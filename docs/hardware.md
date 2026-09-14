# Hardware — Canvas Control Square, Light Squares, PSU

State of public knowledge as of 2026-09-14. Evidence tags: see [README](../README.md#evidence-tags).

## TL;DR

- Canvas is FCC ID **2AEWY-NL29** (granted 2019-05-20). The FCC internal photos are public and show readable chip markings.
- Control Square main SoC: **MediaTek MT7688AN** (MIPS 24KEc, OpenWrt-class) on a plug-in module, with **64 MB DDR2** and **32 MB SPI NOR**.
- Radio co-processor: **Silicon Labs EFR32BG1B232** — Bluetooth LE only, **no 802.15.4**, so no Thread/Matter-over-Thread is possible on this hardware.
- Supply is **42 V DC** (24 W adapter). Every edge has a **3-contact** connector: GND, ~40 V, and a per-side data line silkscreened **EDGE1..EDGE4**.
- A community teardown ([mabels/nanoleaf-canvas-NL29](https://github.com/mabels/nanoleaf-canvas-NL29)) shows silkscreen for `PANEL_RX/TX`, `RHYTHM_TX/RX`, `TOUCH_I2C_SDA/SCL` and an EFR32 SWD header.
- The platform (MT7688AN + Silabs radio + 42 V + 3-pin "EDGE" connectors + same PCB maker) matches **Nanoleaf Shapes**, whose panel bus has been reverse engineered — see [panel-bus.md](panel-bus.md).

## 1. Model numbers and certification

| Item | Value | Tag | Source |
|---|---|---|---|
| FCC grantee | `2AEWY` = NANOGRID LIMITED (Hong Kong), Nanoleaf's certification entity | A | [fccid.io/2AEWY](https://fccid.io/2AEWY) |
| Canvas FCC ID | `2AEWY-NL29` "Nanoleaf Canvas", granted 2019-05-20 | A | [fccid.io/2AEWY-NL29](https://fccid.io/2AEWY-NL29) |
| ISED (Canada) | `20489-NL29` | A | Test report RDG180730066-00B |
| Model tested | `NL29-0003SW-9PK`; family `NL29-XXXXSX-9PK` (X1–X4 region codes, X5 colour; electrically identical) | A | [Similarity declaration](https://fccid.io/2AEWY-NL29/Letter/Product-Similarity-Declaration-4285492) |
| Retail kits | `NL29-0002SW-9PK` (9-square Smarter Kit), `NL29-0001SW-4PK` (4-square expansion) | B | Retail listings |
| Control Square | No separate FCC ID — certified as part of the NL29 kit ("Controller" in exhibits) | A | [Ext. photos](https://fccid.io/2AEWY-NL29/External-Photos/Ext-Photos-4285494) |
| Replacement Control Square SKUs | `NF029P03-1CCP` (US/CA), `NF029P02-1CCP` (EU/UK) | B | nanoleaf.me page titles (pages not fetchable) |
| "60V Control Square" | Title on Nanoleaf US shop; contradicts the 42 V label. Unverified | B, doubtful | [us-shop.nanoleaf.me](https://us-shop.nanoleaf.me/products/canvas-control-square) |
| MAC OUI | `00:55:DA:50/28` registered to Nanoleaf (label shows `00:55:DA:50:21:84`) | A | FCC ext. photo; [Wireshark manuf](https://www.wireshark.org/download/automated/data/manuf) |
| Linker part numbers | Not found | — | — |

**Radios (FCC grant)** [A]: 2402–2480 MHz under Part 15.249 (BLE) and 2412–2462 MHz Wi-Fi b/g/n under Part 15.247. Two internal FPC antennas, 2.15 dBi. Wi-Fi 24 dBm conducted max. Test lab: Bay Area Compliance Laboratories (Dongguan), reports RDG180730066-00A (BLE) and -00B (Wi-Fi); sample received 2018-10-10.

Test-report hints [A → C]: the Wi-Fi was driven with "MT7603 QA V0.0.0.71" (MediaTek QA tool); the BLE radio was driven with "SecureCRT.exe" — suggests the EFR32 exposes a UART test/command interface.

Schematics, block diagram, operational description and BOM are under long-term FCC confidentiality; not available.

## 2. Control Square electronics

Chip markings read from the FCC internal photos ([exhibit 4285496](https://fccid.io/2AEWY-NL29/Internal-Photos/Int-Photos-4285496)) and cross-checked against the [mabels teardown photos](https://github.com/mabels/nanoleaf-canvas-NL29).

| Function | Marking | Identification | Tag |
|---|---|---|---|
| Main SoC / Wi-Fi | `MEDIATEK MT7688AN 1822-AJCSL` | MT7688AN: MIPS 24KEc @ 580 MHz, 1T1R 802.11n. OpenWrt target `ramips/mt76x8` | A |
| Wi-Fi module | Silkscreen `MT7688 V5.4`, PCB maker "JOVE" (module JVE-M2 per mabels) | Plug-in castellated module with printed antenna | A |
| RAM | `ESMT M14D5121632A -2.5B` | 512 Mbit (64 MB) DDR2, 1.8 V | A |
| Flash | GigaDevice `25Q256DYIG` | GD25Q256D, 256 Mbit (32 MB) SPI NOR | A (package code C) |
| BLE co-processor | `EFR32 BG1B232GG 1822C00P29` | EFR32BG1B232F256GM32 — Blue Gecko Series 1, 256 kB flash, **BLE only**. 38.4 MHz crystal, meander antenna | A |
| Microphone | Metal can with sound port next to the EFR32 | MEMS mic for built-in Rhythm (sound reactive) | A (mabels) / C (function) |
| Touch buttons | Copper flex with 6 pads (6 printed icons) + 5 small LEDs; silkscreen `TOUCH_I2C_SDA/SCL` | Capacitive buttons on an I²C touch controller; controller IC not identified | A (parts) / C |
| LED drivers | "four channel mosfet drivers for the LEDs" | — | B (mabels) |
| Unidentified MCU (U1) | TSSOP next to a 6-pad programming header, near the EDGE header | Candidate panel-bus MCU (cf. the EFM8BB10 on Shapes) | A (exists) / C (role) |
| Board revisions | Main PCB `PCB-FZ027-V0.386 2018-09-17`; LED sub-board `PCB-FZ026-V0.8`; maker mark `E356428 SY-JLH-02 94V-0` (same as the NL28 Rhythm module) | — | A |

### Silkscreen of interest (from mabels teardown)

| Label(s) | Notes |
|---|---|
| `GND` / `40V` / `EDGEn` next to each side connector | One data net per side (`EDGE1`..`EDGE4`), not a shared bus pin. FCC photos label the sides `Side 0`..`Side 3` |
| 6-pad header: `EDGE2, GND, 3.3V, EDGE3, EDGE1, EDGE4` | Convenient tap point for all four data lines; suggests 3.3 V logic [C] |
| `PANEL_RX`, `PANEL_TX` | UART from a host chip to the panel-bus logic [C] |
| `RHYTHM_TX`, `RHYTHM_RX` | UART to the sound-reactive block [C] |
| `DBG_RESET, DBG_SWDIO, DBG_SWCLK, VMCU, RXD, TXD` | EFR32 SWD + UART debug header |

### Candidate debug points (none with a confirmed pinout)

- Unpopulated 3-hole footprint on the MT7688 module next to the crystal — likely the SoC serial console [C]. Aurora's RT5350 console runs at 57600 baud ([OpenWrt forum](https://forum.openwrt.org/t/nanoleaf-light-panels/81748)); MT7688 U-Boot commonly uses 57600 or 115200.
- EFR32 SWD/UART header (labels above).
- 6-pad header next to U1 — programming header for the unidentified MCU [C].
- 4-hole rows front and back, purpose unknown.

### Power behaviour

- "The Control Square can draw power from any Light Square in the layout or from a power supply directly." [A, FCC user manual](https://fccid.io/2AEWY-NL29/User-Manual/User-Manual-4285502)
- mabels reports the controller runs stably on 3.3 V injected after its regulator, and that panels can be attached using only GND + signal from any edge [B].

## 3. Light Square (panel)

| Fact | Tag | Source |
|---|---|---|
| 150 × 150 mm; one 3-contact linker slot per side (4 total), confirmed on the bench; one neighbour (square, PSU or controller) per side | A | FCC ext./int. photos; bench 2026-09-14 |
| PCB with 4 angled "LED leaf" sub-boards edge-lighting a light guide; diffuser split into four quadrants | A | FCC int. photos, mabels |
| Roughly 5–6 side-emitting LED packages per leaf (count from photos) | C | mabels photos |
| Large copper pinwheel on the PCB back — likely the capacitive touch electrode | C | FCC int. photos |
| Small IC cluster with an unreadable square IC — the panel MCU | A (exists) / C (role) | FCC int. photos |
| RGB + white LEDs; the white channel is used for panel-side white balancing | A | [OpenAPI docs](https://gist.github.com/dennishn/ae06bf574748727bcce5127394a8ba43) |
| Patent: square luminaire split into 4 light bodies, LEDs hidden at one edge of each, 225 cm² | A (patent) / C (matches Canvas) | [US20210165154A1](https://patents.google.com/patent/US20210165154A1/en) |
| Patent: squares with touch sensors; controller polls each unit for touch data | A (patent) | [US11910507B2](https://patents.google.com/patent/US11910507B2/en) |

**Not public:** panel MCU part number, LED part numbers and count, LED driver, panel-side debug pads.

## 4. Power supply and connector

| Item | Value | Tag | Source |
|---|---|---|---|
| FCC-tested adapter | `DSL-24WF-42`; in 100–120 V~ 0.8 A; out **+42 V ⎓ 0.57 A, 24 W**; Class 2; UL E81356 | A | FCC ext. photos p.5, test report p.4 |
| Adapter cable | 2.5 m, captive; flat plug inserts into a panel linker slot | A | Test report, ext. photos |
| Plug pinout | Label pictogram shows three contacts: **DATA, ⊕, ⊖**. Physical order not readable | A | FCC ext. photos p.5 |
| Retail PSU `NC04-0017` | 25 W, "1 W per square", max 25 Light Squares, Canvas (NL29) only | B | [smart-secure.co.uk](https://www.smart-secure.co.uk/product/nanoleaf-canvas-additional-power-supply/) |
| Other PSU SKUs | `NC04-0016` (24 W), `NC04-0058` (75 W), `NC04-0034` (75 W in-wall) | B | nanoleaf.me titles |
| Max squares per controller | "up to 1000" (marketing) | B | habr.com |
| Linker pinout | Not documented publicly | — | — |

The PSU plug having a DATA contact is notable: on Shapes, PSUs appear as nodes in the layout tree reported over the bus (see [panel-bus.md](panel-bus.md#layout-discovery)) [C for Canvas].

### Bench measurement (2026-09-14)

Canvas Light Square powered by the stock PSU, a linker in a free edge, meter on the linker's exposed pads. "Top/middle/bottom" is the pad order as held during measurement (photo still to be taken).

| Pad | Reading |
|---|---|
| Top → middle | +33 V (black on top, red on middle) |
| Top ↔ bottom | 2.8 V magnitude; probe orientation not recorded |

Supply is in the centre, as the Canvas silkscreen shows (the Shapes ESP32 project's centre = GND does not apply to Canvas). The supply read 33 V, not the 42 V on the adapter label; not yet explained.

**Resolved: bottom = GND, middle = supply, top = DATA** (supply ≈ 35.8 V above GND if the 2.8 V idle adds to the 33 V reading). First wired as top = GND, which made the Pico read the line low almost continuously (`captures/20260914-110340-power-up.json`). With the two outer wires swapped, DATA idles high when powered (`captures/20260914-111610-power-up-swapped.json`) and the square answers commands ([panel-bus.md](panel-bus.md#bench-results-2026-09-14)).

> [!warning] Pin order from sources (superseded by the measurement above)
> Canvas silkscreen reads `GND / 40V / EDGEn` (centre = ~40 V), and mabels says the slightly longer outer pin is GND. The Shapes ESP32 project documents centre = GND. **Measure before connecting anything** — the supply rail is 42 V.

## 5. Sibling products for comparison

| Product | Fact | Tag | Source |
|---|---|---|---|
| Aurora / Light Panels (NL22) controller | Ralink RT5350F, EtronTech EM63A165TS-6G RAM, Winbond SPI flash; power board with 4-pad edge `VBUS EDGE GND 20V`; 24 V supply | A | [FCC NL22 int. photos](https://fccid.io/2AEWY-NL22/Internal-Photos/Int-Photos-3254166) |
| Aurora (NL22) | UART console 57600 baud, OpenWrt Linux 3.18.18, 32 MB RAM / 16 MB flash, password-protected shell | B | [OpenWrt forum](https://forum.openwrt.org/t/nanoleaf-light-panels/81748) |
| Rhythm module (NL28, for Aurora) | Silicon Labs EFR32BG1P332, 38.4 MHz crystal, 24 V input | A | [FCC NL28 int. photos](https://fccid.io/2AEWY-NL28/Internal-Photos/Int-Photos-3964895) |
| Shapes (NL42) controller | MT7688AN (OpenWrt, Wi-Fi) + EFR32MG21 (BLE/Thread) + **EFM8BB10F8G (panel bus)** | B (video teardown) | [TechteamGB](https://techteamgb.co.uk/2024/05/24/trying-to-reverse-engineer-the-nanoleaf-shapes-controller/) |
| Shapes (NL42) | 3-pin edge: 42 V, GND, data (3.3 V, silkscreen `EDGE`); 74LVC1G125/126 auto-direction buffer | A | [Panton](https://christian.panton.org/posts/nanoleaf-ctrl-pt1/) |
| Lines (NL59) | FCC: "WIFI/Thread/2.4G Proprietary"; controller PCB silkscreen `42V` | A | [FCC NL59 int. photos](https://fccid.io/2AEWY-NL59/Internal-Photos/Internal-Photos-5488970) |
| NL06A Wi-Fi module (2024) | TR7628DA4G(LN), MT7628DAN-based, 25 pins, 3.3 V, UART0/1, I²C — possibly the successor module for newer controllers [C] | A | [FCC NL06A manual](https://fccid.io/2AEWY-NL06A) |
| NL04A module (2022) | EFR32MG24 BLE/Thread module | A | [FCC NL04A datasheet](https://fccid.io/2AEWY-NL04A/User-Manual/User-Manual-6058035) |

Other Nanoleaf FCC IDs: NL14 Smarter Hub (2016), NL26 Remote (2018), NL45 Essentials bulb, NL55 Essentials strip (2020), NL01A Essentials module (2021), NL64 Skylight (2024). All under [fccid.io/2AEWY](https://fccid.io/2AEWY).

## 6. Not found anywhere

- A real Canvas teardown beyond FCC photos and the mabels repo (nothing on iFixit, Hackaday, EEVblog, Reddit).
- Panel MCU, LED driver, touch controller IC, DC-DC regulator part numbers.
- Linker pinout and physical pin order.
- Debug header pinouts, boot log, U-Boot/OpenWrt version on a Canvas.
- Hardware revisions after 2018 (the "updated" Control Square SKU on nanoleaf.me EU hints one exists [C]).
- The EEVblog "Nanoleaf Aurora teardown" thread (CAPTCHA-blocked) and Nanoleaf's own community threads (403).
