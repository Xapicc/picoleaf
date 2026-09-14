# Prototype v1 — Pico bus probe

A Raspberry Pi Pico W firmware plus host tool for talking to a Canvas Light Square over its data line when the protocol is unknown. It transmits bytes as a single-wire UART and records every level change on the same wire, so any reply can be decoded afterwards.

v1 does not light panels on its own. It is the instrument for finding out how to. With it, one square was set to a chosen colour on 2026-09-14; the working command sequence is in [panel-bus.md](panel-bus.md#session-reads-brightness-and-colour).

## What is in the repo

| Path | What |
|---|---|
| `pico/src/bus.pio` | PIO programs: push-pull TX, open-drain TX, edge recorder |
| `pico/src/main.c` | USB command interface, DMA plumbing |
| `pico/test/test_pio.py` | Cycle-level emulation of the assembled PIO programs (waveforms, driver release, edge timing) |
| `tools/canvasbus.py` | Host tool: sends commands, saves captures, decodes UART |
| `tools/test_canvasbus.py` | Decoder tests |
| `captures/` | Every capture as `.json` (raw) + `.vcd` (open in PulseView) |

## Python setup

Dependencies are in `pyproject.toml`, managed with uv:

```sh
uv sync                 # creates .venv with pyserial, pytest, ruff
uv run pytest           # decoder + PIO emulation tests
uv run ruff format tools pico/test && uv run ruff check tools pico/test
```

`seq` runs several frames over one serial connection with controlled gaps, which is needed to keep a session alive:

```sh
.venv/bin/python tools/canvasbus.py seq 00 80 C0 'FC 04 FF' 'E0 01 05 00 FF 00 00 00' --label red
```

## Hardware

Raspberry Pi Pico W (RP2040), one resistor (220–470 Ω, 330 Ω is ideal), a spare linker, wire, a Light Square, the stock PSU, a multimeter.

```text
                     Light Square
                 ┌───────────────────┐
  Canvas PSU ───►│ edge A      edge B│◄── linker ── GND ───────────── Pico pin 3 (GND)
                 │                   │         ├─ DATA ──[330 Ω]─── Pico pin 4 (GP2)
                 └───────────────────┘         └─ ~40 V ── not connected, insulated
```

The Pico is powered and controlled over USB from the Mac. GP2 is an input with no pull at boot and is only driven during a `tx` command.

## Step 1 — identify the linker contacts (multimeter)

Sources disagree on which contact is which, and the supply is 42 V. Do this before anything touches the Pico.

1. PSU unplugged. Solder a short wire to each of the three pads on the half of the linker that stays outside the square. Call them A, B, C in order and take a photo.
2. Push the linker into a free edge of the square. Plug the PSU into another edge and power it.
3. Meter on DC volts. Measure A–B, B–C and A–C. One pair reads about 40–42 V; those are GND and V+. The remaining wire is **DATA**.
4. On that pair, put the black probe on one and the red on the other. **+40 V** means black is GND. **−40 V** means red is GND.
5. Black probe on GND, red on DATA: note the idle voltage. It must be **between 0 and 3.3 V** to continue. If it is higher, stop here.
6. Note whether the square lights up or does anything on PSU power alone.
7. Unplug the PSU. Cut the V+ wire short and insulate it.

## Step 2 — flash and self-test the Pico (no square attached)

Build (already done once, repeat after changes):

```sh
export PICO_SDK_PATH=~/.pico-sdk/sdk/2.3.1
export PICO_TOOLCHAIN_PATH=~/.pico-sdk/toolchain/arm-gnu-toolchain-14.3.rel1-darwin-arm64-arm-none-eabi
cmake -S pico -B pico/build -G Ninja && cmake --build pico/build
```

Flash: hold BOOTSEL while plugging in the Pico, then `picotool load -x pico/build/canvas_probe.uf2`, or drag the `.uf2` onto the `RPI-RP2` drive. Once the probe firmware is running, later flashes work with `picotool load -f -x pico/build/canvas_probe.uf2` without pressing BOOTSEL.

Self-test with nothing connected to GP2:

```sh
.venv/bin/python tools/canvasbus.py info
.venv/bin/python tools/canvasbus.py send pull up     # info should now show level=1
.venv/bin/python tools/canvasbus.py tx 5 55 AA E0 --label selftest
```

The `tx` capture must decode as `tx  55 AA E0` with no `!` marks. That confirms the transmitter and the edge recorder work on real silicon.

**Result on the Pico W (RP2040 B2), 2026-09-14:** passed with nothing attached to GP2.

| Mode | Baud | Frame | Result |
|---|---|---|---|
| push-pull | 1 000 000 | `55 AA E0 03 05 05 FF 00 00 00` | decoded, no stop-bit errors; bits measured at 1.0 µs, stop bits 1.5 µs as designed |
| push-pull | 115 200 | `55 AA E0 03` | decoded |
| open-drain + internal pull-up | 115 200 | `55 AA E0 03` | decoded |
| open-drain + internal pull-up | 1 000 000 | `55 AA E0 03` | decoded, but high pulses shrink to ~0.7 µs: the ~50 kΩ internal pull-up is too weak at this rate. Open-drain at 1 Mbaud relies on the panel's own pull-up |

The first flash failed silently: the three PIO programs need 43 instructions and one PIO block holds 32, so the recorder never loaded. The recorder now runs on `pio1`, and the firmware panics at boot if a program doesn't fit.

## Step 3 — connect and listen

Order matters: **GND first, then DATA**. Keep the PSU unplugged while connecting.

```sh
.venv/bin/python tools/canvasbus.py send pull none
.venv/bin/python tools/canvasbus.py cap 8000 --label power-up    # plug the PSU in during these 8 s
.venv/bin/python tools/canvasbus.py cap 2000 --label idle
.venv/bin/python tools/canvasbus.py info                         # idle level without a pull
```

This shows whether a square talks on its own at power-up or while idle, and the line's resting level.

## Step 4 — probe with the Shapes hypothesis

These frames come from the reverse-engineered Shapes bus ([panel-bus.md](panel-bus.md#command-set)). None of them are known for Canvas. Only these benign opcodes are used; `FE` (panel firmware update on Shapes) is blocked by the tool.

```sh
T=".venv/bin/python tools/canvasbus.py"
$T tx 50 00                         --label root-detect
$T tx 50 80                         --label layout
$T tx 20 C0                         --label poll
$T tx 50 FC 04 FF                   --label brightness
$T tx 50 E0 03 05 05 FF 00 00 00    --label colour-red
```

Watch the square while each runs. In the output, look for `rx` bursts after the `tx` line.

If nothing answers, repeat the set with other line settings:

```sh
$T send mode od && $T send pull up          # open-drain with pull-up
$T send baud 115200                         # Aurora-era rate
```

Decode a saved capture differently without re-running it:

```sh
$T decode captures/<file>.json --baud 115200
$T decode captures/<file>.json --inverted
```

## Firmware command reference

Text lines over USB serial; each reply ends with `OK` or `ERR <reason>`.

| Command | Effect |
|---|---|
| `info` | Version, pin, baud, drive mode, pull, current level |
| `level` | Current line level |
| `diag` | Bring-up check of the edge recorder with and without DMA |
| `baud <bps>` | TX bit rate, 1200–3000000 (default 1000000) |
| `mode pp\|od` | Push-pull (drives high and low, releases after the frame) or open-drain (only pulls low) |
| `pull up\|down\|none` | RP2040 internal pull (~50 kΩ) |
| `cap <ms>` | Record for up to 30 s |
| `tx <ms> <hex…>` | Start recording, send up to 512 bytes, keep recording `<ms>` after the frame |

Capture limits: 40,000 level changes per capture, 16 ns resolution (62.5 MHz ticks), timing drift-free across edges (verified in emulation, not yet on hardware).
