# PicoLeaf

**Run a Nanoleaf Canvas light wall from a $7 Raspberry Pi Pico W — no Nanoleaf controller, no app, no cloud, straight into Home Assistant.**

[![CI](https://github.com/Xapicc/picoleaf/actions/workflows/ci.yml/badge.svg)](https://github.com/Xapicc/picoleaf/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A Nanoleaf Canvas (model NL29) is a wall of touch-sensitive light squares. They are dumb on their own: the brain is the **Control Square**, one special panel in the box that talks to the others over a private wired bus and to your phone over Wi-Fi. Lose it, break it, or buy squares second-hand without one, and the wall stays dark — no public decode of that bus existed when this project started.

PicoLeaf is a replacement brain. A Raspberry Pi Pico W, wired into one square through a single resistor, finds every square on the wall, drives each one's colour 25 times a second, and announces itself to Home Assistant over MQTT. The protocol was worked out from scratch on the bench with the Pico as its own logic analyser; every claim in [`docs/`](docs/) carries the capture it came from.

Running a 13-square wall since 2026-09-14. Firmware 0.7.0.

## What it does

- **Every square addressed individually.** The Pico reads the wall's shape at power-up — which square sits where, and which way round — and notices squares being added or removed while it runs. Verified on 1, 2 and 13 squares.
- **Appears in Home Assistant by itself.** MQTT discovery creates one RGB light per square plus one for the whole wall, with no custom component and no YAML. Entity identity comes from each square's 16-byte hardware ID, so rearranging the wall doesn't break your automations.
- **Eight effects**, three of them position-aware (rainbow wave, ripple, fire and friends). They run on the Pico at 25 Hz; nothing streams over the network.
- **Smooth transitions** for single squares and the wall, usable from scenes and automations.
- **Stands on its own.** On a phone charger, with no computer attached, it rejoins Wi-Fi and the broker by itself, and blinks the wall red when MQTT has been down for 30 seconds.
- **Holds up.** A 10-minute stress run pushed 14,963 colour frames at 24.9 Hz with no lost session and a worst-case poll gap of 40 ms.

Not there: touch input, the Nanoleaf app, HomeKit, Matter, sound-reactive effects, and streaming from other software — see [scope](docs/scope.md#out-of-scope). The squares' white channel is reachable over USB but not exposed to Home Assistant.

## What you need

| Item | Note |
|---|---|
| Canvas Light Squares, stock 42 V PSU, linkers | The Control Square is not used and need not exist |
| Raspberry Pi Pico W | About $7. It is the only active part: bus master, logic analyser and Wi-Fi bridge |
| One 330 Ω resistor (220–470 Ω), a spare linker, some wire | The entire interface circuit |
| A multimeter | To find which contact carries 42 V before anything touches the Pico |
| 2.4 GHz Wi-Fi, an MQTT broker, Home Assistant | Only for the Home Assistant half; the wall can also be driven over USB |

> [!WARNING]
> **The panel rail is 42 V DC, on the middle contact — right between the two you solder to.** The RP2040 is not 5 V tolerant, and while the Pico is on USB your computer is in the fault path. Identify every contact with a meter before connecting anything: [prototype-v1.md, step 1](docs/prototype-v1.md#step-1--identify-the-linker-contacts-multimeter).

## How it works

```text
    ┌──────────┐   Wi-Fi    ┌─────────────┐   MQTT    ┌────────────────┐
    │  Pico W  │───────────►│ MQTT broker │──────────►│ Home Assistant │
    └────┬─────┘            └─────────────┘           └────────────────┘
         │
         │   the three contacts of one linker, pushed into any square's edge:
         │
         │   GP2 ──[ 330 Ω ]──── DATA   (top pad)
         │                       42 V   (middle pad) — cut short, insulated
         │   GND ─────────────── GND    (bottom pad)
         │
    ┌────┴──────┬───────────┬───────────┬── … up to a whole wall
    │ square 0  │ square 1  │ square 2  │        ▲
    └───────────┴───────────┴───────────┘        └── stock 42 V PSU, in any edge
```

Each square edge has three contacts: one data line and GND on the outside, **the 42 V supply in the middle, between the two wires you actually use**. Only the outer two reach the Pico; the supply wire is cut short and insulated. Which outer pad is which is not printed anywhere, so it has to be metered — on this bench it came out as DATA on top and GND on the bottom.

Squares speak a **single-wire half-duplex UART at 1 Mbaud, 3.3 V**, relayed neighbour to neighbour. They never start an exchange — they only answer. So the Pico:

1. **opens a session** (`00`, then `80`) and gets back a depth-first description of the wall's shape,
2. **polls** (`C0`) at least every 450 ms, or the squares drop the session,
3. **pushes colour** 25 times a second: one `E0 01` frame carrying one RGBW entry per square, farthest square first.

Fades and effects are computed on the Pico rather than by the panels, because the panels' own transition unit turned out to be minutes long. The full decode, with the captures behind each step, is in [panel-bus.md](docs/panel-bus.md).

## Getting started

**1. Get the firmware.** Download `canvas_probe.uf2` from the [latest release](https://github.com/Xapicc/picoleaf/releases/latest), or build it (Pico SDK 2.3.1, `arm-none-eabi`, Ninja):

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S pico -B pico/build -G Ninja && cmake --build pico/build
```

CI also builds it on every push, as a workflow artifact.

**2. Flash it.** Hold BOOTSEL while plugging the Pico in, then drag the `.uf2` onto the `RPI-RP2` drive (or `picotool load -x canvas_probe.uf2`). Self-test it with nothing connected — [step 2](docs/prototype-v1.md#step-2--flash-and-self-test-the-pico-no-square-attached).

**3. Wire it up**, meter first: [step 1](docs/prototype-v1.md#step-1--identify-the-linker-contacts-multimeter) and [step 3](docs/prototype-v1.md#step-3--connect-and-listen). GND before DATA, PSU unplugged.

**4. Put it on the network.** Host tools need Python ≥ 3.9 and [uv](https://docs.astral.sh/uv/):

```sh
uv sync
uv run pytest      # PIO emulation tests are skipped until the firmware has been built once
uv run python tools/canvasbus.py provision --ssid <your-ssid> --mqtt-host <broker> --mqtt-user canvas
```

`provision` asks for the two passwords without echoing them and stores them in the Pico's flash — never in this repo. Home Assistant picks the wall up within a few seconds. Details, topics and broker setup: [home-assistant.md](docs/home-assistant.md).

Without a network, `canvasbus.py` drives the wall directly over USB: `fill`, `set <i>`, `frame`, `bright`, `layout`, `stats`. The command reference is at the end of [prototype-v1.md](docs/prototype-v1.md#firmware-command-reference).

## Docs

| File | Covers |
|---|---|
| [docs/scope.md](docs/scope.md) | Goal, definition of done, stretch goals, out of scope, decisions, work plan, risks |
| [docs/prototype-v1.md](docs/prototype-v1.md) | Pico W firmware: wiring, pin identification, flashing, bring-up runbook, command reference |
| [docs/home-assistant.md](docs/home-assistant.md) | Home Assistant over MQTT: broker setup, provisioning, topics, entities, effects, verification |
| [docs/panel-bus.md](docs/panel-bus.md) | Controller ↔ panel protocol: the Canvas decode, the Shapes bus it started from, Aurora, captures |
| [docs/hardware.md](docs/hardware.md) | FCC filings, chips on the Control Square, Light Squares, PSU, debug pads, sibling products |
| [docs/network-api.md](docs/network-api.md) | The original controller's network side: discovery, auth, REST, extControl v2, touch events |
| [docs/firmware-and-security.md](docs/firmware-and-security.md) | Firmware history, update delivery, OS evidence, CVEs, reset/pairing, GPL status |
| [docs/open-questions.md](docs/open-questions.md) | What is still unknown — touch, hot-plug signalling, the transition unit — and what has been answered |
| `captures/` | Every bench capture as `.json` (raw) plus `.vcd` (open in PulseView) |
| `firmware/canvas/` | 63 Canvas firmware images (1.1.0–12.4.1), `SHA256SUMS`, `manifest.tsv`. The images are git-ignored |

## Findings worth knowing

- **Canvas squares speak the Shapes panel protocol.** No public work existed for Canvas, but the reverse-engineered [Shapes](https://christian.panton.org/posts/nanoleaf-ctrl-pt1/) bus turned out to be a working hypothesis: 1 Mbaud 8N1, single wire, controller-polled, relayed, depth-first layout report.
- **Layout.** The layout reply encodes each square's entry side and its neighbours clockwise from there, depth-first, with the PSU reported as a one-edge node and `40` closing every open branch. Parsed blind on a 13-square wall, it matched the physical wall exactly.
- **Colour.** `E0 01` takes one 6-byte RGBW entry per square in reverse order; a frame with the wrong number of entries is silently ignored. Reply trailers are CRC-16/ARC.
- **Quirks.** A session dies after ~480 ms without a poll. Squares power up at about half brightness and forget global brightness when their power is cut, so it is re-sent with every colour frame.
- **Platform.** The Control Square runs a MediaTek MT7688AN (MIPS, OpenWrt-class) with an EFR32BG1B BLE co-processor and a MEMS mic, read from public FCC photos (FCC ID `2AEWY-NL29`). Canvas never got Thread or Matter; its radio cannot do 802.15.4.

## Standing on the shoulders of

| Who | What | Product |
|---|---|---|
| [Christian Panton](https://christian.panton.org/posts/nanoleaf-ctrl-pt1/) | Panel bus protocol decode, traced edge schematic | Shapes |
| [mabels/nanoleaf-canvas-NL29](https://github.com/mabels/nanoleaf-canvas-NL29) | Control Square teardown photos with readable silkscreen | Canvas |
| [FCC 2AEWY-NL29](https://fccid.io/2AEWY-NL29) | Internal/external photos, test reports, manual | Canvas |
| [TechteamGB](https://techteamgb.co.uk/2024/05/24/trying-to-reverse-engineer-the-nanoleaf-shapes-controller/) + [captures](https://github.com/andymanic/NanoleafShapesRE) | Controller teardown, raw logic captures | Shapes |
| [TotalPanther317/Nanoleaf-controller](https://github.com/TotalPanther317/Nanoleaf-controller) | ESP32 replacement controller (waveform replay) | Shapes |
| [kronoshacker](https://kronoshacker.blogspot.com/2018/01/playing-with-aurora-led-panels.html) | Panel bus protocol and connector pinout | Aurora |
| [dagbdagb/nanoleaf-firmware-upgrade-notes](https://github.com/dagbdagb/nanoleaf-firmware-upgrade-notes) | Firmware URLs, local upload page | Panel family |
| [pwning.tech CVE-2022-47758](https://pwning.tech/cve-2022-47758/) | OpenWrt/MT76x8 internals, cloud MQTT daemon | Panel family |
| [DMXControl NanoleafAPI](https://github.com/patrick-dmxc/NanoleafAPI) | Undocumented API commands, tested on Canvas | Canvas |
| [Nanoleaf OpenAPI docs](https://nanoleaf.atlassian.net/wiki/spaces/nlapid/pages/2789310530/Nanoleaf+Light+Panels+Open+API+Documentation) | Official API spec | Panel family |

## Evidence tags

Every claim in `docs/` carries one, so you can tell measurement from guesswork.

| Tag | Meaning |
|---|---|
| **A** | Confirmed from a primary source: official docs, FCC exhibits, datasheets, published captures/code/photos, or a bench measurement in this repo |
| **B** | Community claim without evidence we could check (write-ups without raw data, forum posts, single-source code) |
| **C** | Inference drawn while compiling these notes; treat as a hypothesis |

A few sources could not be read during the survey (an EEVblog teardown behind a CAPTCHA, Nanoleaf community threads returning 403, Reddit). Those gaps are listed in each doc.

## License

MIT, see [LICENSE](LICENSE). `pico/pico_sdk_import.cmake` is copied from the Pico SDK and keeps its BSD-3-Clause license.

This is an independent project, not affiliated with or endorsed by Nanoleaf. "Nanoleaf" and "Canvas" are trademarks of their owner. Nanoleaf firmware images are not distributed here. Opening a light wall's bus is at your own risk: 42 V, and no warranty of any kind.
