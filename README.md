# FckAhLeaf

Reverse engineering the Nanoleaf Canvas (NL29) Control Square.

**Scope:** drive Canvas Light Squares with a Raspberry Pi Pico W (RP2040), with no Nanoleaf Control Square attached. The Pico is the only active hardware; it also serves as the logic analyser. The definition of done, stretch goals, non-goals and work plan are in [docs/scope.md](docs/scope.md).

So far this repo holds a survey of existing public work (compiled 2026-09-14) and an archive of Canvas firmware images. There are no original measurements yet.

## Docs

| File | Covers |
|---|---|
| [docs/scope.md](docs/scope.md) | Goal, definition of done, stretch goals, out of scope, decisions, work plan, risks |
| [docs/prototype-v1.md](docs/prototype-v1.md) | Pico W bus probe: wiring, pin identification, flashing, bring-up runbook, command reference |
| [docs/hardware.md](docs/hardware.md) | FCC filings, chips on the Control Square, Light Squares, PSU, debug pads, sibling products |
| [docs/panel-bus.md](docs/panel-bus.md) | Controller ↔ panel protocol: Canvas evidence, the reverse-engineered Shapes bus, Aurora bus, capture plan |
| [docs/network-api.md](docs/network-api.md) | Discovery, auth, REST API, extControl v2 streaming, SSE/UDP touch events, undocumented commands, client libraries |
| [docs/firmware-and-security.md](docs/firmware-and-security.md) | Firmware history, update delivery, OS evidence, CVEs, reset/pairing, GPL status |
| [docs/open-questions.md](docs/open-questions.md) | Unknowns, grouped by whether they block the scope |
| `firmware/canvas/` | 63 Canvas firmware images (1.1.0–12.4.1), `SHA256SUMS`, `manifest.tsv`. Images are git-ignored |

## Key findings

- **Platform.** The Control Square runs a **MediaTek MT7688AN** (MIPS, OpenWrt-class) with 64 MB DDR2 and 32 MB SPI NOR, plus a **Silicon Labs EFR32BG1B232** BLE-only co-processor and a MEMS mic. Read from public FCC photos (FCC ID `2AEWY-NL29`).
- **Edge connectors.** Each edge has 3 contacts: GND, ~40 V (42 V PSU), and a per-side data line silkscreened `EDGE1..EDGE4`. Pin order is unverified.
- **Panel bus.** No public protocol work exists for Canvas. The **Shapes** bus is documented (1 Mbaud single-wire UART, controller-polled, relayed, DFS layout report), and Canvas shares its physical design, so that is the starting hypothesis.
- **Local API.** HTTP on 16021, extControl v2 over UDP 60222 (16-bit panelIds, RGBW, transition × 100 ms), SSE events, UDP touch stream. Well documented, with known doc errors noted in [network-api.md](docs/network-api.md).
- **Firmware.** Images are downloadable over plain HTTP from `canvas-firmware.s3.amazonaws.com`. Nothing is public on signing or the OS; siblings run OpenWrt. No GPL source has been released.
- **Thread/Matter.** Canvas never received Thread or Matter, and its radio cannot do 802.15.4.

## Most useful prior work

| Who | What | Product |
|---|---|---|
| [mabels/nanoleaf-canvas-NL29](https://github.com/mabels/nanoleaf-canvas-NL29) | Control Square teardown photos with readable silkscreen | Canvas |
| [FCC 2AEWY-NL29](https://fccid.io/2AEWY-NL29) | Internal/external photos, test reports, manual | Canvas |
| [Christian Panton](https://christian.panton.org/posts/nanoleaf-ctrl-pt1/) | Panel bus protocol decode, traced edge schematic | Shapes |
| [TechteamGB](https://techteamgb.co.uk/2024/05/24/trying-to-reverse-engineer-the-nanoleaf-shapes-controller/) + [captures](https://github.com/andymanic/NanoleafShapesRE) | Controller teardown, raw logic captures | Shapes |
| [TotalPanther317/Nanoleaf-controller](https://github.com/TotalPanther317/Nanoleaf-controller) | ESP32 replacement controller (waveform replay) | Shapes |
| [kronoshacker](https://kronoshacker.blogspot.com/2018/01/playing-with-aurora-led-panels.html) | Panel bus protocol and connector pinout | Aurora |
| [dagbdagb/nanoleaf-firmware-upgrade-notes](https://github.com/dagbdagb/nanoleaf-firmware-upgrade-notes) | Firmware URLs, local upload page | Panel family |
| [pwning.tech CVE-2022-47758](https://pwning.tech/cve-2022-47758/) | OpenWrt/MT76x8 internals, cloud MQTT daemon | Panel family |
| [DMXControl NanoleafAPI](https://github.com/patrick-dmxc/NanoleafAPI) | Undocumented API commands, tested on Canvas | Canvas |
| [Nanoleaf OpenAPI docs](https://nanoleaf.atlassian.net/wiki/spaces/nlapid/pages/2789310530/Nanoleaf+Light+Panels+Open+API+Documentation) | Official API spec | Panel family |

## Evidence tags

Every claim in `docs/` carries a tag.

| Tag | Meaning |
|---|---|
| **A** | Confirmed from a primary source: official docs, FCC exhibits, datasheets, published captures/code/photos |
| **B** | Community claim without evidence we could check (write-ups without raw data, forum posts, single-source code) |
| **C** | Inference drawn while compiling these notes; treat as a hypothesis |

Several sources could not be read during the survey (EEVblog Aurora teardown behind a CAPTCHA, Nanoleaf community threads returning 403, Reddit). Those gaps are listed in each doc.

> [!warning] Safety
> The panel rail is 42 V DC. Identify pins with a meter before attaching a logic analyser or microcontroller.
