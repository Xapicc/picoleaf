// Canvas panel controller and bus probe for the Raspberry Pi Pico W (RP2040).
//
// At power-up the controller (controller.c) opens a session with the squares,
// polls them and pushes colours set over USB or from Home Assistant via MQTT
// (net.c, home_assistant.c). The probe commands transmit
// bytes as a single-wire half-duplex UART on the same GPIO and record every
// level change at ~16 ns resolution for decoding on the host
// (tools/canvasbus.py); they pause the controller. Commands are text lines over
// USB CDC; every command answers with a final "OK" or "ERR <reason>" line.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "pico/unique_id.h"

#include "bus.pio.h"
#include "config_flash.h"
#include "config_record.h"
#include "controller.h"
#include "home_assistant.h"
#include "net.h"
#include "uart_decode.h"

#define FIRMWARE_VERSION "0.6.1"

// GP2 is header pin 4, next to GND on pin 3.
#define BUS_PIN 2

// 64 KB of the RP2040's 264 KB SRAM, leaving room for Wi-Fi and lwIP. One word per level change.
#define CAPTURE_WORDS 16000
#define MAX_TX_BYTES 512
#define MAX_LISTEN_MS 30000
#define MIN_BAUD 1200
#define MAX_BAUD 3000000
#define COMMAND_LINE_MAX 4096

// Stop bits are stretched by the TX program's FIFO checks; 12 bits per byte
// is a safe upper bound for how long a transmission occupies the line.
#define TX_BITS_PER_BYTE_BOUND 12

typedef enum { DRIVE_PUSH_PULL, DRIVE_OPEN_DRAIN } drive_mode_t;
typedef enum { PULL_NONE, PULL_UP, PULL_DOWN } pull_mode_t;

// The three programs need 43 instructions and a PIO block holds 32, so the
// recorder runs on pio1. PIO inputs see every GPIO whatever its function.
static const PIO tx_pio = pio0;
static const PIO capture_pio = pio1;
static const uint TX_SM = 0;
static const uint CAPTURE_SM = 0;

static uint tx_pp_offset;
static uint tx_od_offset;
static uint capture_offset;
static uint tx_dma;
static uint capture_dma;

static uint32_t baud = 1000000;
static drive_mode_t drive_mode = DRIVE_PUSH_PULL;
static pull_mode_t pull_mode = PULL_NONE;

static controller_t controller;
static device_config_t device_config;
static bool network_ready;

static uint32_t capture_buffer[CAPTURE_WORDS];
static uint32_t tx_words[MAX_TX_BYTES];

static uint add_program_or_panic(PIO pio, const pio_program_t *program) {
    int offset = pio_add_program(pio, program);
    if (offset < 0) {
        panic("pio_add_program failed (%d): not enough free instructions in PIO%u", offset, pio_get_index(pio));
    }
    return (uint)offset;
}

static const char *drive_mode_name(void) {
    return drive_mode == DRIVE_OPEN_DRAIN ? "od" : "pp";
}

static const char *pull_mode_name(void) {
    switch (pull_mode) {
        case PULL_UP: return "up";
        case PULL_DOWN: return "down";
        default: return "none";
    }
}

static void apply_pull(void) {
    gpio_set_pulls(BUS_PIN, pull_mode == PULL_UP, pull_mode == PULL_DOWN);
}

static void tx_sm_configure(void) {
    bool open_drain = drive_mode == DRIVE_OPEN_DRAIN;
    uint offset = open_drain ? tx_od_offset : tx_pp_offset;
    pio_sm_config config = open_drain ? bus_tx_od_program_get_default_config(offset)
                                      : bus_tx_pp_program_get_default_config(offset);
    sm_config_set_out_pins(&config, BUS_PIN, 1);
    sm_config_set_set_pins(&config, BUS_PIN, 1);
    if (!open_drain) {
        sm_config_set_sideset_pins(&config, BUS_PIN);
        sm_config_set_mov_status(&config, STATUS_TX_LESSTHAN, 1);
    }
    sm_config_set_out_shift(&config, true, false, 32);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&config, (float)clock_get_hz(clk_sys) / (8.0f * (float)baud));

    pio_sm_set_enabled(tx_pio, TX_SM, false);
    // Preset the output latch before the program can enable the driver:
    // idle-high for push-pull, low for open-drain (it only ever pulls down).
    pio_sm_set_pins_with_mask(tx_pio, TX_SM, open_drain ? 0 : 1u << BUS_PIN, 1u << BUS_PIN);
    pio_sm_set_pindirs_with_mask(tx_pio, TX_SM, 0, 1u << BUS_PIN);
    pio_sm_init(tx_pio, TX_SM, offset, &config);
    pio_sm_set_enabled(tx_pio, TX_SM, true);
}

static void capture_sm_configure(void) {
    pio_sm_config config = edge_capture_program_get_default_config(capture_offset);
    sm_config_set_jmp_pin(&config, BUS_PIN);
    sm_config_set_in_pins(&config, BUS_PIN);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv_int_frac8(&config, 1, 0);
    pio_sm_init(capture_pio, CAPTURE_SM, capture_offset, &config);
}

static void dma_configure(void) {
    // DMA feeds the TX FIFO so a CPU interrupt (USB) can never open a gap in
    // the middle of a frame, which would release the line early.
    tx_dma = (uint)dma_claim_unused_channel(true);
    dma_channel_config tx_config = dma_channel_get_default_config(tx_dma);
    channel_config_set_transfer_data_size(&tx_config, DMA_SIZE_32);
    channel_config_set_read_increment(&tx_config, true);
    channel_config_set_write_increment(&tx_config, false);
    channel_config_set_dreq(&tx_config, pio_get_dreq(tx_pio, TX_SM, true));
    dma_channel_set_config(tx_dma, &tx_config, false);
    dma_channel_set_write_addr(tx_dma, &tx_pio->txf[TX_SM], false);

    capture_dma = (uint)dma_claim_unused_channel(true);
    dma_channel_config capture_config = dma_channel_get_default_config(capture_dma);
    channel_config_set_transfer_data_size(&capture_config, DMA_SIZE_32);
    channel_config_set_read_increment(&capture_config, false);
    channel_config_set_write_increment(&capture_config, true);
    channel_config_set_dreq(&capture_config, pio_get_dreq(capture_pio, CAPTURE_SM, false));
    dma_channel_set_config(capture_dma, &capture_config, false);
    dma_channel_set_read_addr(capture_dma, &capture_pio->rxf[CAPTURE_SM], false);
}

static void capture_start(void) {
    pio_sm_set_enabled(capture_pio, CAPTURE_SM, false);
    pio_sm_clear_fifos(capture_pio, CAPTURE_SM);
    pio_sm_restart(capture_pio, CAPTURE_SM);
    pio_sm_exec(capture_pio, CAPTURE_SM, pio_encode_jmp(capture_offset));
    capture_pio->fdebug = 1u << (PIO_FDEBUG_RXSTALL_LSB + CAPTURE_SM);

    dma_channel_set_write_addr(capture_dma, capture_buffer, false);
    dma_channel_set_transfer_count(capture_dma, dma_encode_transfer_count(CAPTURE_WORDS), true);
    pio_sm_set_enabled(capture_pio, CAPTURE_SM, true);
}

// Returns the number of words recorded; *truncated is set when the buffer
// filled up and edges after that point were lost.
static uint32_t capture_stop(bool *truncated) {
    pio_sm_set_enabled(capture_pio, CAPTURE_SM, false);
    while (!pio_sm_is_rx_fifo_empty(capture_pio, CAPTURE_SM) && dma_channel_is_busy(capture_dma)) {
    }
    // Errata RP2040-E13: aborting while a word is read but not yet written
    // misbehaves. The FIFO is empty now, so let the last write land first.
    sleep_us(100);
    dma_channel_abort(capture_dma);

    uint32_t words = (dma_hw->ch[capture_dma].write_addr - (uintptr_t)capture_buffer) / sizeof(uint32_t);
    bool stalled = capture_pio->fdebug & (1u << (PIO_FDEBUG_RXSTALL_LSB + CAPTURE_SM));
    *truncated = stalled || words >= CAPTURE_WORDS;
    return words;
}

static void print_capture(uint32_t words, bool truncated, uint64_t start_us, uint64_t tx_us,
                          uint64_t stop_us, size_t tx_count) {
    uint32_t start_level = words > 0 ? capture_buffer[0] : 0;
    printf("CAP version=%s pin=%u baud=%lu mode=%s pull=%s start_level=%lu tick_hz=%lu "
           "edges=%lu start_us=%llu tx_us=%llu stop_us=%llu tx_bytes=%u truncated=%d\n",
           FIRMWARE_VERSION, BUS_PIN, (unsigned long)baud, drive_mode_name(), pull_mode_name(),
           (unsigned long)start_level, (unsigned long)(clock_get_hz(clk_sys) / 2),
           (unsigned long)(words > 0 ? words - 1 : 0), (unsigned long long)start_us,
           (unsigned long long)tx_us, (unsigned long long)stop_us, (unsigned)tx_count, truncated);

    // The PIO pushes countdown timestamps; print how many ticks each level
    // lasted. Unsigned subtraction also covers the counter wrapping.
    uint32_t previous = 0xFFFFFFFFu;
    for (uint32_t i = 1; i < words; i++) {
        bool line_start = (i - 1) % 16 == 0;
        bool line_end = (i - 1) % 16 == 15 || i == words - 1;
        uint32_t ticks = previous - capture_buffer[i];
        previous = capture_buffer[i];
        printf("%s%lu%s", line_start ? "D " : " ", (unsigned long)ticks, line_end ? "\n" : "");
    }
    printf("END\n");
}

typedef struct {
    uint32_t words;
    bool truncated;
    uint64_t start_us;
    uint64_t tx_us;
    uint64_t stop_us;
} transaction_t;

// Records the line while sending `bytes`, and for `listen_ms` after the frame.
static transaction_t transact(const uint8_t *bytes, size_t count, uint32_t listen_ms) {
    transaction_t result = {0};
    capture_start();
    result.start_us = time_us_64();
    result.tx_us = result.start_us;

    if (count > 0) {
        for (size_t i = 0; i < count; i++) {
            tx_words[i] = drive_mode == DRIVE_OPEN_DRAIN ? (uint8_t)~bytes[i] : bytes[i];
        }
        dma_channel_set_read_addr(tx_dma, tx_words, false);
        result.tx_us = time_us_64();
        dma_channel_set_transfer_count(tx_dma, dma_encode_transfer_count(count), true);
    }

    uint64_t tx_duration_us = (uint64_t)count * TX_BITS_PER_BYTE_BOUND * 1000000ull / baud;
    sleep_until(from_us_since_boot(result.tx_us + tx_duration_us + (uint64_t)listen_ms * 1000ull));

    result.words = capture_stop(&result.truncated);
    result.stop_us = time_us_64();
    return result;
}

static void run_transaction(const uint8_t *bytes, size_t count, uint32_t listen_ms) {
    transaction_t result = transact(bytes, count, listen_ms);
    print_capture(result.words, result.truncated, result.start_us, result.tx_us, result.stop_us, count);
}

// controller_exchange_fn: sends a frame and decodes the reply on the Pico.
static size_t bus_exchange(const uint8_t *frame, size_t length, uint32_t listen_ms, uint8_t *reply, size_t reply_max) {
    transaction_t result = transact(frame, length, listen_ms);
    static uint8_t values[CONTROLLER_LAYOUT_MAX + MAX_TX_BYTES];
    static bool stop_ok[CONTROLLER_LAYOUT_MAX + MAX_TX_BYTES];
    size_t decoded = uart_decode(capture_buffer, result.words, clock_get_hz(clk_sys) / 2, baud, values, stop_ok,
                                 sizeof values);
    // Our own frame is recorded too; everything after it is the panels' answer.
    if (decoded <= length) return 0;
    size_t reply_length = decoded - length;
    if (reply_length > reply_max) reply_length = reply_max;
    memcpy(reply, values + length, reply_length);
    return reply_length;
}

static void resume_controller(void) {
    // The controller speaks the protocol measured at 1 Mbaud push-pull.
    baud = 1000000;
    drive_mode = DRIVE_PUSH_PULL;
    tx_sm_configure();
    controller.enabled = true;
    controller.in_session = false;
    controller.next_session_attempt_us = time_us_64();
}

// Probe commands would interleave with controller traffic, so they pause it.
static void pause_controller(void) {
    if (!controller.enabled) return;
    controller.enabled = false;
    printf("CTL paused for probe command; 'ctl on' resumes\n");
}

static void report_controller_event(controller_event_t event) {
    switch (event) {
        case CONTROLLER_SESSION_OPENED:
            printf("CTL session open, %u squares\n", (unsigned)controller.square_count);
            break;
        case CONTROLLER_SESSION_FAILED:
            printf("CTL no valid layout reply, retrying in 1 s\n");
            break;
        case CONTROLLER_SESSION_LOST:
            printf("CTL session lost, re-reading layout\n");
            break;
        case CONTROLLER_UIDS_READY:
            printf("CTL hardware IDs read for %u squares\n", (unsigned)controller.square_count);
            break;
        default:
            break;
    }
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Accepts tokens like "80", "E003", "0xE0". Returns byte count or -1.
static int parse_hex_tokens(char *save, uint8_t *out, size_t max) {
    size_t count = 0;
    for (char *token = strtok_r(NULL, " \t", &save); token; token = strtok_r(NULL, " \t", &save)) {
        if (token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) token += 2;
        size_t length = strlen(token);
        if (length == 0 || length % 2 != 0) return -1;
        for (size_t i = 0; i < length; i += 2) {
            int high = hex_value(token[i]);
            int low = hex_value(token[i + 1]);
            if (high < 0 || low < 0 || count >= max) return -1;
            out[count++] = (uint8_t)(high << 4 | low);
        }
    }
    return (int)count;
}

static bool parse_uint(const char *text, uint32_t *value) {
    if (!text || !*text) return false;
    char *end;
    unsigned long parsed = strtoul(text, &end, 10);
    if (*end != '\0') return false;
    *value = (uint32_t)parsed;
    return true;
}

// Bring-up aid: runs the edge recorder once without DMA (reading the FIFO
// directly) and once with DMA, and prints the internals of both paths.
static void print_diagnostics(void) {
    pio_sm_set_enabled(capture_pio, CAPTURE_SM, false);
    pio_sm_clear_fifos(capture_pio, CAPTURE_SM);
    pio_sm_restart(capture_pio, CAPTURE_SM);
    pio_sm_exec(capture_pio, CAPTURE_SM, pio_encode_jmp(capture_offset));
    pio_sm_set_enabled(capture_pio, CAPTURE_SM, true);
    sleep_ms(5);
    printf("DIAG pio: pc=%u (offset %u) rx_level=%u fdebug=0x%08lx ctrl=0x%08lx\n",
           pio_sm_get_pc(capture_pio, CAPTURE_SM), capture_offset, pio_sm_get_rx_fifo_level(capture_pio, CAPTURE_SM),
           (unsigned long)capture_pio->fdebug, (unsigned long)capture_pio->ctrl);
    pio_sm_set_enabled(capture_pio, CAPTURE_SM, false);
    while (!pio_sm_is_rx_fifo_empty(capture_pio, CAPTURE_SM)) {
        printf("DIAG pio word 0x%08lx\n", (unsigned long)pio_sm_get(capture_pio, CAPTURE_SM));
    }

    capture_start();
    sleep_ms(5);
    dma_channel_hw_t *channel = dma_channel_hw_addr(capture_dma);
    printf("DIAG dma: ch=%u ctrl=0x%08lx remaining=%lu written=%ld busy=%d rx_level=%u\n", capture_dma,
           (unsigned long)channel->ctrl_trig, (unsigned long)channel->transfer_count,
           (long)((channel->write_addr - (uintptr_t)capture_buffer) / 4), dma_channel_is_busy(capture_dma),
           pio_sm_get_rx_fifo_level(capture_pio, CAPTURE_SM));
    bool truncated;
    uint32_t words = capture_stop(&truncated);
    printf("DIAG after stop: words=%lu first=0x%08lx\n", (unsigned long)words, (unsigned long)capture_buffer[0]);
}

// Parses RRGGBB or RRGGBBWW.
static bool parse_colour(const char *text, uint8_t rgbw[4]) {
    if (!text) return false;
    size_t length = strlen(text);
    if (length != 6 && length != 8) return false;
    rgbw[3] = 0;
    for (size_t i = 0; i < length; i += 2) {
        int high = hex_value(text[i]);
        int low = hex_value(text[i + 1]);
        if (high < 0 || low < 0) return false;
        rgbw[i / 2] = (uint8_t)(high << 4 | low);
    }
    return true;
}

// Decodes a hex string into a NUL-terminated string. Values travel hex-encoded so
// SSIDs and passwords may contain spaces or any other character.
static bool decode_hex_string(const char *hex, char *out, size_t out_size) {
    if (hex == NULL) return false;
    size_t length = strlen(hex);
    if (length % 2 != 0 || length / 2 >= out_size) return false;
    for (size_t i = 0; i < length; i += 2) {
        int high = hex_value(hex[i]);
        int low = hex_value(hex[i + 1]);
        if (high < 0 || low < 0) return false;
        out[i / 2] = (char)(high << 4 | low);
    }
    out[length / 2] = '\0';
    return true;
}

// Returns false if `command` is not a network or settings command.
static bool handle_network_command(const char *command, char *save) {
    if (strcmp(command, "cfg") == 0) {
        char *action = strtok_r(NULL, " \t", &save);
        if (action && strcmp(action, "set") == 0) {
            char *key = strtok_r(NULL, " \t", &save);
            char value[128];
            if (!key || !decode_hex_string(strtok_r(NULL, " \t", &save), value, sizeof value) ||
                !config_set_field(&device_config, key, value)) {
                printf("ERR usage: cfg set <wifi_ssid|wifi_password|mqtt_host|mqtt_port|mqtt_user|mqtt_password|"
                       "layout_rotation> <hex value>\n");
                return true;
            }
        } else if (action && strcmp(action, "show") == 0) {
            // Passwords are never echoed back.
            printf("CFG wifi_ssid=%s wifi_password=%s mqtt_host=%s mqtt_port=%u mqtt_user=%s mqtt_password=%s "
                   "layout_rotation=%u\n",
                   device_config.wifi_ssid, device_config.wifi_password[0] ? "set" : "unset", device_config.mqtt_host,
                   device_config.mqtt_port, device_config.mqtt_user, device_config.mqtt_password[0] ? "set" : "unset",
                   device_config.layout_rotation);
        } else if (action && strcmp(action, "save") == 0) {
            if (!config_flash_save(&device_config)) {
                printf("ERR writing settings to flash failed\n");
                return true;
            }
            printf("CFG saved; 'reboot' to apply\n");
        } else {
            printf("ERR usage: cfg set <key> <hex> | cfg show | cfg save\n");
            return true;
        }
    } else if (strcmp(command, "net") == 0) {
        char status[256];
        if (network_ready) {
            net_describe(status, sizeof status);
        } else {
            snprintf(status, sizeof status, "state=wifi-chip-unavailable");
        }
        printf("NET %s\n", status);
    } else if (strcmp(command, "reboot") == 0) {
        printf("OK\n");
        stdio_flush();
        watchdog_reboot(0, 0, 100);
        for (;;) sleep_ms(1000);
    } else {
        return false;
    }
    printf("OK\n");
    return true;
}

// Returns false if `command` is not a controller command.
static bool handle_controller_command(const char *command, char *save) {
    uint8_t rgbw[4];
    if (strcmp(command, "ctl") == 0) {
        char *state = strtok_r(NULL, " \t", &save);
        if (state && strcmp(state, "on") == 0) {
            resume_controller();
        } else if (state && strcmp(state, "off") == 0) {
            controller.enabled = false;
        } else {
            printf("ERR usage: ctl on|off\n");
            return true;
        }
    } else if (strcmp(command, "fill") == 0) {
        if (!parse_colour(strtok_r(NULL, " \t", &save), rgbw)) {
            printf("ERR usage: fill RRGGBB[WW]\n");
            return true;
        }
        controller_fill(&controller, rgbw);
    } else if (strcmp(command, "set") == 0) {
        uint32_t index;
        if (!parse_uint(strtok_r(NULL, " \t", &save), &index) || !parse_colour(strtok_r(NULL, " \t", &save), rgbw) ||
            !controller_set(&controller, index, rgbw)) {
            printf("ERR usage: set <index 0..%d> RRGGBB[WW]\n", CONTROLLER_MAX_SQUARES - 1);
            return true;
        }
    } else if (strcmp(command, "frame") == 0) {
        size_t index = 0;
        for (char *token = strtok_r(NULL, " \t", &save); token; token = strtok_r(NULL, " \t", &save), index++) {
            if (!parse_colour(token, rgbw) || !controller_set(&controller, index, rgbw)) {
                printf("ERR frame entry %u: expected RRGGBB[WW], at most %d entries\n", (unsigned)index,
                       CONTROLLER_MAX_SQUARES);
                return true;
            }
        }
    } else if (strcmp(command, "bright") == 0) {
        uint32_t level;
        if (!parse_uint(strtok_r(NULL, " \t", &save), &level) || level > 255) {
            printf("ERR usage: bright <0..255>\n");
            return true;
        }
        controller_set_brightness(&controller, (uint8_t)level);
    } else if (strcmp(command, "stats") == 0) {
        char *action = strtok_r(NULL, " \t", &save);
        if (action && strcmp(action, "reset") == 0) {
            memset(&controller.stats, 0, sizeof controller.stats);
        } else {
            const controller_stats_t *stats = &controller.stats;
            printf("STATS enabled=%d session=%d squares=%u sessions_opened=%lu session_failures=%lu sessions_lost=%lu "
                   "frames_sent=%lu polls_ok=%lu max_poll_gap_us=%llu last_bad_poll_length=%lu uids_read=%u "
                   "uid_read_failures=%lu uptime_us=%llu\n",
                   controller.enabled, controller.in_session, (unsigned)controller.square_count,
                   (unsigned long)stats->sessions_opened, (unsigned long)stats->session_failures,
                   (unsigned long)stats->sessions_lost, (unsigned long)stats->frames_sent,
                   (unsigned long)stats->polls_ok, (unsigned long long)stats->max_poll_gap_us,
                   (unsigned long)stats->last_bad_poll_length, (unsigned)controller.uids_read,
                   (unsigned long)stats->uid_read_failures,
                   (unsigned long long)time_us_64());
        }
    } else if (strcmp(command, "layout") == 0) {
        printf("LAYOUT squares=%u bytes=", (unsigned)controller.square_count);
        for (size_t i = 0; i < controller.layout_length; i++) printf("%02X", controller.layout[i]);
        printf("\n");
    } else {
        return false;
    }
    printf("OK\n");
    return true;
}

static void print_info(void) {
    printf("INFO version=%s pin=%u baud=%lu mode=%s pull=%s level=%d sys_hz=%lu capture_words=%u ctl=%s\n",
           FIRMWARE_VERSION, BUS_PIN, (unsigned long)baud, drive_mode_name(), pull_mode_name(),
           gpio_get(BUS_PIN), (unsigned long)clock_get_hz(clk_sys), CAPTURE_WORDS, controller.enabled ? "on" : "off");
}

static void handle_command(char *line) {
    char *save = NULL;
    char *command = strtok_r(line, " \t", &save);
    if (!command) return;
    if (handle_controller_command(command, save)) return;
    if (handle_network_command(command, save)) return;

    bool probe_uses_bus = strcmp(command, "diag") == 0 || strcmp(command, "baud") == 0 ||
                          strcmp(command, "mode") == 0 || strcmp(command, "cap") == 0 || strcmp(command, "tx") == 0;
    if (probe_uses_bus) pause_controller();

    if (strcmp(command, "info") == 0) {
        print_info();
    } else if (strcmp(command, "diag") == 0) {
        print_diagnostics();
    } else if (strcmp(command, "level") == 0) {
        printf("LEVEL %d\n", gpio_get(BUS_PIN));
    } else if (strcmp(command, "baud") == 0) {
        uint32_t requested;
        if (!parse_uint(strtok_r(NULL, " \t", &save), &requested) || requested < MIN_BAUD || requested > MAX_BAUD) {
            printf("ERR baud must be %d..%d\n", MIN_BAUD, MAX_BAUD);
            return;
        }
        baud = requested;
        tx_sm_configure();
    } else if (strcmp(command, "mode") == 0) {
        char *mode = strtok_r(NULL, " \t", &save);
        if (mode && strcmp(mode, "pp") == 0) {
            drive_mode = DRIVE_PUSH_PULL;
        } else if (mode && strcmp(mode, "od") == 0) {
            drive_mode = DRIVE_OPEN_DRAIN;
        } else {
            printf("ERR mode must be pp or od\n");
            return;
        }
        tx_sm_configure();
    } else if (strcmp(command, "pull") == 0) {
        char *pull = strtok_r(NULL, " \t", &save);
        if (pull && strcmp(pull, "up") == 0) {
            pull_mode = PULL_UP;
        } else if (pull && strcmp(pull, "down") == 0) {
            pull_mode = PULL_DOWN;
        } else if (pull && strcmp(pull, "none") == 0) {
            pull_mode = PULL_NONE;
        } else {
            printf("ERR pull must be up, down or none\n");
            return;
        }
        apply_pull();
    } else if (strcmp(command, "cap") == 0) {
        uint32_t listen_ms;
        if (!parse_uint(strtok_r(NULL, " \t", &save), &listen_ms) || listen_ms > MAX_LISTEN_MS) {
            printf("ERR usage: cap <ms 0..%d>\n", MAX_LISTEN_MS);
            return;
        }
        run_transaction(NULL, 0, listen_ms);
    } else if (strcmp(command, "tx") == 0) {
        uint32_t listen_ms;
        if (!parse_uint(strtok_r(NULL, " \t", &save), &listen_ms) || listen_ms > MAX_LISTEN_MS) {
            printf("ERR usage: tx <ms 0..%d> <hex bytes>\n", MAX_LISTEN_MS);
            return;
        }
        static uint8_t bytes[MAX_TX_BYTES];
        int count = parse_hex_tokens(save, bytes, MAX_TX_BYTES);
        if (count <= 0) {
            printf("ERR expected 1..%d hex bytes\n", MAX_TX_BYTES);
            return;
        }
        run_transaction(bytes, (size_t)count, listen_ms);
    } else if (strcmp(command, "help") == 0) {
        printf("controller: ctl on|off | fill RRGGBB[WW] | set <i> RRGGBB[WW] | frame RRGGBB[WW]... | bright <n> | "
               "stats [reset] | layout\n"
               "network: cfg set <key> <hex> | cfg show | cfg save | net | reboot\n"
               "probe: info | level | baud <bps> | mode pp|od | pull up|down|none | cap <ms> | tx <ms> <hex...> | diag\n");
    } else {
        printf("ERR unknown command '%s' (try help)\n", command);
        return;
    }
    printf("OK\n");
}

int main(void) {
    stdio_init_all();

    // The pin comes up as an input with no pull; the controller starts driving
    // it once the probe hardware is set up.
    pio_gpio_init(tx_pio, BUS_PIN);
    apply_pull();

    // Claim our state machines before the Wi-Fi driver picks a free one for its own PIO program.
    pio_sm_claim(tx_pio, TX_SM);
    pio_sm_claim(capture_pio, CAPTURE_SM);
    tx_pp_offset = add_program_or_panic(tx_pio, &bus_tx_pp_program);
    tx_od_offset = add_program_or_panic(tx_pio, &bus_tx_od_program);
    capture_offset = add_program_or_panic(capture_pio, &edge_capture_program);
    dma_configure();
    tx_sm_configure();
    capture_sm_configure();
    controller_init(&controller, time_us_64());

    config_defaults(&device_config);
    if (!config_flash_load(&device_config)) printf("CFG no saved settings; network stays off until 'cfg save'\n");

    char board_id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(board_id, sizeof board_id);
    for (char *c = board_id; *c; c++) *c = (char)tolower((unsigned char)*c);
    home_assistant_init(&controller, &device_config, board_id, FIRMWARE_VERSION, time_us_64());

    if (cyw43_arch_init() != 0) {
        printf("NET wifi chip init failed; running without network\n");
    } else {
        network_ready = net_init(&device_config, home_assistant_client_id(), home_assistant_subscription(),
                                 home_assistant_availability_topic(), home_assistant_on_message);
        if (!network_ready) printf("NET could not allocate the MQTT client; running without network\n");
    }

    static char line[COMMAND_LINE_MAX];
    size_t length = 0;
    bool overflow = false;
    for (;;) {
        uint64_t now_us = time_us_64();
        home_assistant_render(now_us);
        controller_event_t event = controller_tick(&controller, now_us, bus_exchange);
        report_controller_event(event);
        home_assistant_on_controller_event(event, now_us);
        if (network_ready) {
            net_poll(now_us);
            home_assistant_poll(now_us);
        }

        int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) {
            sleep_us(200);  // don't spin: controller deadlines are tens of ms apart
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (overflow) {
                printf("ERR line longer than %d characters\n", COMMAND_LINE_MAX - 1);
            } else if (length > 0) {
                line[length] = '\0';
                handle_command(line);
            }
            length = 0;
            overflow = false;
        } else if (length + 1 < COMMAND_LINE_MAX) {
            line[length++] = (char)c;
        } else {
            overflow = true;
        }
    }
}
