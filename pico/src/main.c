// Canvas panel bus probe for the Raspberry Pi Pico W (RP2040).
//
// Transmits bytes as a single-wire half-duplex UART on one GPIO and records
// every level change on that same GPIO at ~16 ns resolution. The protocol is
// unknown, so decoding happens on the host from the raw edge timings
// (tools/canvasbus.py). Commands are text lines over USB CDC; every command
// answers with a final "OK" or "ERR <reason>" line.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"

#include "bus.pio.h"

#define FIRMWARE_VERSION "0.3.0"

// GP2 is header pin 4, next to GND on pin 3.
#define BUS_PIN 2

// 160 KB of the RP2040's 264 KB SRAM. One word per level change.
#define CAPTURE_WORDS 40000
#define MAX_TX_BYTES 512
#define MAX_LISTEN_MS 30000
#define MIN_BAUD 1200
#define MAX_BAUD 3000000
#define LINE_MAX 4096

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

static void print_info(void) {
    printf("INFO version=%s pin=%u baud=%lu mode=%s pull=%s level=%d sys_hz=%lu capture_words=%u\n",
           FIRMWARE_VERSION, BUS_PIN, (unsigned long)baud, drive_mode_name(), pull_mode_name(),
           gpio_get(BUS_PIN), (unsigned long)clock_get_hz(clk_sys), CAPTURE_WORDS);
}

static void handle_command(char *line) {
    char *save = NULL;
    char *command = strtok_r(line, " \t", &save);
    if (!command) return;

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
        printf("info | level | baud <bps> | mode pp|od | pull up|down|none | cap <ms> | tx <ms> <hex...> | diag\n");
    } else {
        printf("ERR unknown command '%s' (try help)\n", command);
        return;
    }
    printf("OK\n");
}

int main(void) {
    stdio_init_all();

    // The pin comes up as an input with no pull: nothing is driven until a
    // command asks for it.
    pio_gpio_init(tx_pio, BUS_PIN);
    apply_pull();

    tx_pp_offset = add_program_or_panic(tx_pio, &bus_tx_pp_program);
    tx_od_offset = add_program_or_panic(tx_pio, &bus_tx_od_program);
    capture_offset = add_program_or_panic(capture_pio, &edge_capture_program);
    dma_configure();
    tx_sm_configure();
    capture_sm_configure();

    static char line[LINE_MAX];
    size_t length = 0;
    bool overflow = false;
    for (;;) {
        int c = getchar();
        if (c == '\r' || c == '\n') {
            if (overflow) {
                printf("ERR line longer than %d characters\n", LINE_MAX - 1);
            } else if (length > 0) {
                line[length] = '\0';
                handle_command(line);
            }
            length = 0;
            overflow = false;
        } else if (length + 1 < LINE_MAX) {
            line[length++] = (char)c;
        } else {
            overflow = true;
        }
    }
}
