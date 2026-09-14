# Panel bus — controller ↔ Light Square communication

State of public knowledge as of 2026-09-14. Evidence tags: see [README](../README.md#evidence-tags).

## TL;DR

- **No public capture, decode or replacement controller exists for the Canvas panel bus.** What exists for Canvas is physical evidence (FCC photos, teardown silkscreen) and the OpenAPI's view of panels.
- **Shapes (NL42) / Elements** is the best-documented Nanoleaf bus: single-wire half-duplex **UART 8N1 at 1 Mbaud, 3.3 V**, controller-initiated, relayed panel to panel, with a depth-first layout report. There are published byte captures and an ESP32 replacement controller.
- **Aurora / Light Panels (NL22)** used a different design: 4-pin connector, shared 24 V open-collector bus at 115200 8N1, per-edge "select" pin for topology.
- Canvas shares Shapes' 3-pin, 42 V, per-side `EDGE` design and MT7688AN + Silabs platform, so **the working hypothesis is a Shapes-like protocol** — to be verified by capture, not assumed.

## 1. Canvas (NL29) — what is known

### Physical / electrical

| Fact | Tag | Source |
|---|---|---|
| 3 contacts per edge, one linker slot per side, 4 per square (confirmed on the bench) | A | FCC photos, [mabels](https://github.com/mabels/nanoleaf-canvas-NL29), bench |
| Controller silkscreen per side: `GND / 40V / EDGEn` → supply on centre pin, data on an outer pin | A (photo) / C (reading) | mabels |
| "The slider with the 3 pins uses the slightly longer pin is GND and on the other outer side, that's signal" | B | mabels |
| Separate data net per side (`EDGE1`..`EDGE4`) → point-to-point per edge, not a shared bus pin | A (silkscreen) | mabels |
| 6-pad header `EDGE2, GND, 3.3V, EDGE3, EDGE1, EDGE4` → 3.3 V logic on EDGE lines | A / C | mabels |
| `PANEL_RX` / `PANEL_TX` silkscreen → a UART feeds the panel-bus logic (possibly the unidentified U1 MCU) | A / C | mabels |
| PSU plug has a `DATA` contact | A | FCC ext. photos |
| Panels can be attached using only GND + signal from any edge | B | mabels |
| 42 V DC supply, 24 W adapter | A | FCC test report |

### From the OpenAPI (official) — constraints the bus must satisfy

Source: [Nanoleaf OpenAPI docs mirror](https://gist.github.com/dennishn/ae06bf574748727bcce5127394a8ba43). Details in [network-api.md](network-api.md).

- Canvas uses extControl **v2**: 16-bit panelId, R G B W, 16-bit transition time in 100 ms units. "100ms transitions are handled at a panel level" → **fades are computed in panel firmware**, so the bus carries targets + transition time, not per-frame intermediate colours [A/C].
- Layout `shapeType`: 2 = Square, 3 = Control Square Master, 4 = Control Square Passive [A]. `numPanels` includes the Control Square.
- Canvas panelIds are 16-bit values (e.g. 46095, 31391); Aurora's are small (107, 114) — consistent with Aurora's 1-byte bus short ID, and implying a different ID scheme on Canvas [C].
- Community note (defunct forum, snippet only): IDs are "based on when the controller detects the panel and not where" and are preserved [B].
- Touch is reported per panel with type (Hover/Down/Hold/Up/Swipe), 4-bit strength, and "swiped-from" panel → the panel reports touch state and the controller infers swipes, or panels report it directly [C].

### Bench results (2026-09-14)

First original measurements, from one Light Square on the stock PSU driven by the Pico W probe ([prototype-v1.md](prototype-v1.md)): GP2 → 330 Ω → DATA, push-pull, 1 Mbaud 8N1. Raw captures in `captures/`. Tag A for what was observed; interpretations are C.

| Observation | Capture |
|---|---|
| DATA idles **high** once powered and the square sends **nothing on its own**: no traffic in 16.5 s after power-up | `20260914-111610-power-up-swapped` |
| Unpowered, the line floats (50 Hz mains pickup with no pull) | same, first 13.4 s |
| `00` (Shapes root side detect): no reply | `probe2-pp-1M-pull*-root-detect` |
| **`80` (layout): reply `C3` `00` `93` `00` `40`**, identical with and without the Pico pull-up. First byte starts 24.5 µs after our stop bit; bytes arrive individually with 19–54 µs gaps; `40` at 226 µs | `probe2-pp-1M-pull*-layout` |
| **`C0` (poll): reply of 2 bytes, `00 00`**, starting 27 µs after our frame, back to back. Once `20 00` directly after a colour frame | `probe2-pp-1M-pull*-poll*` |
| `FC 04 FF` (Shapes global brightness): no reply; **the square's brightness changed** (user observation) | `probe2-pp-1M-pull*-brightness` |
| `E0 03 05 05 FF 00 00 00` (Shapes bulk colour): no reply; visible effect not yet confirmed | `probe2-pp-1M-pull*-colour-red` |

Interpretation against Panton's Shapes layout format [C]:
- `C3`: node header (bit 7 set), edges = `(0xC3 >> 4) & 7` = **4 → a square**, rotation `3`.
- `00`: port-offset byte.
- `93`: node header, edges = **1 → the PSU** (Shapes reports PSUs as 1-edge nodes; a PSU is plugged into this square), rotation `3`.
- `00`: port-offset byte.
- `40`: end of layout, same terminator as Shapes.
- The poll reply is 2 bytes for a 1-square layout, matching Shapes' 2 bytes per panel slot.

So far Canvas squares speak the Shapes panel protocol at 1 Mbaud 8N1 on 3.3 V single-wire logic.

#### Session, reads, brightness and colour

| Finding | Evidence |
|---|---|
| **`80` only answers after `00`.** `80` on its own gets no reply; `00` then `80` returns the layout | `session-layout`, `session-root`, `session-layout-after-root` |
| **A session is `00` + `80`.** Commands only take effect inside one. `FC 04 FF` sent outside a session did nothing; the same frame after `00 80` worked | user observation, `session-*` |
| **`C0` poll answers `CC` outside a session** (Shapes: "hot-swap detected") and 2 bytes inside one | `nosession-poll`, `timeout1-*` |
| **Session times out after ~480–500 ms without a poll.** Gap of 476 ms between polls kept it; 497 ms and longer dropped it. The square keeps its last output when the session drops | `seq` runs with `wait=` 450–530 |
| **`F8 00 00 81` reads the version:** `01` then `04 "0.1.1" 00 FF FF FF FF "4.6.0" 00 00 00` + `F2 8F`. Panel firmware 4.6.0 | `addressed-reads-03` |
| **`F8 00 00 82` reads a 16-byte ID:** `01` then `2D 10 02 11 87 04 26 AF CB 3C B5 5A 05 19 00 F5` + `AA 51` | `addressed-reads-07` |
| **Reply trailer is CRC-16/ARC** (poly 0x8005 reflected, init 0), little-endian, over the bytes after the leading `01`. Verified on both replies | computed |
| Node 1 (the PSU) does not answer `F8 01 00 81/82` | `addressed-reads-05/09` |
| **`FC 04 vv` sets global brightness.** `FF` full, `40` dim, `10` looked off | user observation |
| **`E0 01 05 TT R G B W` sets the square's colour.** `E0 01 05 00 FF 00 00 00` switched to red instantly. `TT` is a panel-side transition with a long, possibly non-linear unit: `01` reached blue (not timed), `03` showed no visible change within 12 s, `05` faded over a minute or more, `0A` showed no visible change within 20 s (13 squares, 2026-09-14). The controller firmware always sends `00` and fades on the Pico instead | user observation, `e001-red-t0`, `e001-blue`, transition sequence `20260914-1351*`–`1352*` |
| `E0 03 05 05 R G B W` (the Shapes capture format) had no visible effect, with or without CRC, PSU slot or `FC 07`/`FC 08` around it | `colour-test*` |
| `E0 02 05 05 FF 00 FF 00` **dropped the session** (next poll `CC`) — probably a longer 16-bit frame format | `colour-test2` |
| The poll reply's first byte varies: `10`, `11`, `12`, `20` seen, otherwise `00`. Possibly touch data (unverified) | several `seq` runs |

Default state of a square powered with no controller: white at about half brightness (user observation).

#### Two squares (square B linked to square A; Pico and PSU on A)

| Finding | Evidence |
|---|---|
| **Layout `C3 00 93 00 C0 00 00 40`**: A (`C3`, 4 edges, rotation 3) `00`, PSU (`93`) `00`, B (`C0`, 4 edges, rotation 0) `00 00`, end `40`. The number of `00` bytes after B presumably encodes which side of A it attaches to (not yet correlated with the physical layout) | `two-tiles-reads-01` |
| **Poll reply is 2 bytes per square**, PSU not counted: `00 00 00 00` | `two-tiles-*` |
| **`F8` index counts squares only**, skipping the PSU node: `F8 01 00 81` reached B. B's replies start with one `00` per relay hop, then `01` | `two-tiles-reads-05`, `-11` |
| B: firmware 4.6.0, ID `26 20 06 13 29 08 2D AF A8 7A 2A 5B 00 19 00 F5` (CRC `BA 0D`) | `two-tiles-reads-11` |
| **`E0 01` takes one 6-byte entry per square, in reverse order**: the first entry goes to the square farthest from the controller (B), the last to A. `E0 01 05 00 FF 00 00 00 05 00 00 FF 00 00` made B red and A green. Matches the TotalPanther ESP32 project's "panels in reverse order" on Shapes | user observation, `two-tiles-order`, `two-tiles-colour` |
| **Frames with the wrong number of entries are ignored**: a single entry with two squares did nothing | same |
| **`01 FF` ("no change" on Shapes) is not accepted** as an entry: `E0 01 01 FF 05 00 …` did nothing | same |

Whether "reverse" means reverse enumeration order or farthest-first by hop count needs a third square to tell apart.

#### Layout encoding

Square A has the Pico on its right side and the PSU on its left side (viewed from the front). One slot per side, one neighbour per side.

| B attached to A's | Layout reply | Capture |
|---|---|---|
| top | `C3 00 93 00 C0 00 00 40` | `two-tiles-reads-01` |
| bottom | `C3 C2 00 00 00 93 00 40` | `layout-b-bottom-left-01` |
| top, re-linked in a different orientation (previous orientation not recorded) | `C3 00 93 00 C1 00 00 40` | `layout-current-01` |

**Blind test on a 13-square wall (2026-09-14).** The Pico was plugged into an existing layout without describing it. Reply:

```text
C1 00 00 C3 C3 C2 00 C3 00 00 93 00 00 C3 C2 00 00 00 00 C0 00 C1 00 00 C2 00 00 C2 00 00 00 00 00 C2 00 C2 00 00 00 00 40
```

`tools/canvasbus.py layout` parsed it with no overlaps or leftover bytes into 13 squares and 1 PSU, and the poll returned 26 bytes (2 per square), matching the count. The user confirmed the drawn shape, neighbours and PSU position; the drawing was upside down because it uses the root square's own frame. With `--rotate 180` it matches the wall exactly, Pico and PSU sides included, so the traversal order is clockwise and not mirrored. This validates the structural model: clockwise side order, depth-first subtrees, `93 00` PSU, `40` as the final entry. Capture: `layout-read` files from that run.

The third reading changes only B's nibble (`C0` → `C1`) while the structure stays identical, which supports the nibble being B's own entry side. Whether sides are numbered clockwise or counter-clockwise is open: rotating a square in place isn't possible on this bench because of the slot positions, so it needs a third square or a different entry side for A.

A model that fits both readings [C, to be tested further]:
- **`C` + side**: a square; the low nibble is the side facing its parent, numbered from the square's own frame: bottom 0, left 1, top 2, right 3. A is entered from its right (`C3`); B on top is entered from its bottom (`C0`); B underneath is entered from its top (`C2`).
- After a square's header come its other three sides **clockwise from the entry side**, each either `00` (empty) or the attached node's full subtree (depth-first).
- The PSU is `93 00`.
- **`40` ends the stream** and closes every open list; sides not yet listed are empty. That is why B on top shows only two `00`, and why A's empty top is missing when B is underneath.

Decoded: top case = A(entry right) → bottom empty, left PSU, top B(entry bottom) → B's left, top empty, [right implied]. Bottom case = A → bottom B(entry top: its left, bottom, right empty), left PSU, [top implied].

Minimal working sequence for one square:

```text
00                        root detect (no reply)
80                        layout -> C3 00 93 00 40
C0                        poll -> 00 00, repeat at least every ~450 ms
FC 04 FF                  full brightness
E0 01 05 00 RR GG BB WW   colour, instant
```

### What does not exist publicly

Any bus capture, baud rate, framing, command set, layout-discovery trace, touch report format, timing, CRC — for Canvas.

## 2. Shapes (NL42) / Elements — the reference protocol

Elements and Shapes share linkers and can be mixed; neither is compatible with Canvas or Light Panels [A, Nanoleaf product info].

Primary sources:
- **[Panton]** Christian Panton, "Nanoleaf Shapes deepdive, part 1", 2025-01-16 — https://christian.panton.org/posts/nanoleaf-ctrl-pt1/ (part 2 promised, not published as of 2026-09)
- **[TTGB]** TechteamGB, 2024-05-24 teardown + captures — https://techteamgb.co.uk/2024/05/24/trying-to-reverse-engineer-the-nanoleaf-shapes-controller/ and raw sigrok captures at https://github.com/andymanic/NanoleafShapesRE
- **[ESP32]** TotalPanther317/Nanoleaf-controller, Jan 2026 — https://github.com/TotalPanther317/Nanoleaf-controller

### Physical / electrical

| Fact | Tag | Source |
|---|---|---|
| 3-pin edge: 42 V, GND, data (`EDGE`), 3.3 V logic | A | Panton, TTGB, ESP32 |
| Spring contacts on ~10 × 1.5 mm pads, 1 mm spacing | B | Panton |
| Controller: MT7688AN + EFR32MG21 + **EFM8BB10F8G driving the panel line** | B | TTGB |
| Controller edge interface: `EDGE` → 2.1 kΩ → 74LVC1G126 (RX) / 74LVC1G125 (TX); tri-state enables driven from TX via diode + 10 kΩ + 220 pF → automatic direction switching | A (traced schematic) | Panton |
| ESP32 wiring: GPIO4 (TX) via 3.3 kΩ + 200 Ω and GPIO16 (RX) via 3.3 kΩ to "Right Pin (DATA)"; "Center Pin (GND)" | A | ESP32 schematic |
| Each panel has its own MCU (type unknown) | B | TTGB |

Open issue [C]: the RC in Panton's direction-switch circuit would hold TX enabled ~1.5–2 ms after TX idles, which conflicts with panel replies arriving ~25 µs after a poll. Either the schematic reading or the component values are off.

### Link layer

| Fact | Tag | Source |
|---|---|---|
| **UART 8N1, 1 000 000 baud**, half-duplex on the single data wire | B (Panton) + independently confirmed by decoding TTGB's 2 MHz captures and by the ESP32 code's 1 µs RMT timing | Panton, TTGB data, ESP32 |
| Controller initiates all traffic; panels only reply to polls | B | Panton |
| Data is **relayed hop by hop** panel to panel, not multidrop | B | Panton |
| TTGB's 115200 assumption (copied from Aurora) is wrong; his 500 kHz captures can't resolve 1 Mbaud | C (pulse-width histograms) | TTGB data |

### Command set

| Opcode | Meaning | Corroboration |
|---|---|---|
| `00` | Root side detect | Seen in ESP32 init waveform |
| `80` | Layout detection; panels stream a DFS-ordered tree terminated by `40` | ESP32 sends it; its sample reply ends `40` |
| `E0 03 …` | Bulk colour push. Per panel: `05 YY b1 b2 b3 b4`, or `01 FF` = no change. Firmware strings hint at `E0 01` (8-bit) and `E0 02` (16-bit) variants | Seen in TTGB captures |
| `C0` | Bulk poll; controller then reads 2 bytes per panel. `CC` = hot-swap detected → triggers layout detection | Seen every ~50 ms in captures |
| `F8 lo hi sub` | Addressed command: sub `81` = firmware version, `82` = UID, `04` = brightness (firmware only). Reply: one `00` per hop, then `01` + payload | Not in captures |
| `FC 04 v` | Broadcast global brightness | ESP32 code |
| `FC 07 0/1` | Broadcast touch off/on (per Panton) | `FC 07 00` seen in captures |
| `FC 08 00` | Broadcast keep-alive | — |
| `FE …` | Panel firmware update and reboot | — |

Boot sequence (Panton): layout detection → query versions → query UIDs. Example addressed version read 8 hops away returns `00`×7, `01`, then `06 "0.7.0" FF FF FF FF "2.3.0" … 88 2B`.

### Layout discovery

Panton's encoding of the `80` reply [B]:

- Node header byte has bit 7 set. Edge count = `(h >> 4) & 7` (6 = hexagon, 1 = PSU). Rotation relative to parent = `h & 0x0F`.
- Zero or more `00`/`01` bytes follow; their count gives the clockwise offset of the attach port relative to the parent.
- Depth-first order; order assigns node IDs. **PSUs are nodes in the tree.**
- Branch-return encoding not worked out.
- Example: `e5 00 e1 00 01 00 00 e4 00 01 00 00 01 95 00 e5 00 01 00 95 00 00`

ESP32 project's stored layout reply decodes as `80 | B1 95 04 E1 00 01 00 00 40` — headers and `40` terminator fit Panton's format; the `04` is unexplained [C].

### Independent decode of TTGB captures (9-slot Shapes install)

Decoded from the public `.sr` files as UART 8N1 LSB-first at 2 samples/bit [C, from raw public data]:

```text
static colour change ("nl green to blue 2mhz"):
  FC 07 00
  E0 03 | 05 05 07 FF 2D 0A | ×9 identical chunks   (56 bytes, 10 µs/byte, no gaps)
  FC 07 00                                          (~1.5 ms later)

poll, every 50.07 ms:
  C0 + 18 bytes; inter-byte gaps alternate ~10–14 µs / ~20–26 µs
  → 2 bytes per panel slot, matching Panton

animation ("northern lights"):
  E0 03 | 05 01 FF 00 96 00 | 05 01 DB 00 B1 0B | …  every ~50 ms (20 Hz),
  each followed ~1.3–2.4 ms later by a C0 poll
```

Interpretation [C]:
- `05` is a length byte (5 bytes follow). `YY` was `05` for static scenes and `01` during animation → plausibly transition time in 100 ms units, like the API's `transitionTime`.
- The 4 payload bytes look like **R G B W**, not "colour + brightness" as Panton suggests: the 50 %-brightness capture has chunks byte-identical to the 100 % one (brightness is global via `FC 04`), and the 4th byte rises as R falls.
- Poll replies are almost all `00`. Two slots consistently read `1F 00` and `14 00`/`13 00`; meaning unknown.
- `FC 07 00` brackets every static push — odd if it really means "touch off".

### ESP32 replacement controller notes

- Init `00`, ~10 ms gap, `80`. Heartbeat `C0` every 50 ms. Brightness `FC 04 v`. Colour `E0 03 05 05 R G B W`.
- It **replays captured waveforms** via RMT rather than using a UART; colour bytes are sent MSB-first (bit-reversed relative to UART) and panels in reverse order. README calls it unreliable; hexagons and mini triangles only.

## 3. Aurora / Light Panels (NL22)

Primary source: kronoshacker, "Playing with the Aurora LED Panels", 2018-01-23 — https://kronoshacker.blogspot.com/2018/01/playing-with-aurora-led-panels.html (detailed, no captures published → B throughout).

| Fact | Tag |
|---|---|
| 4-pin connector: 24 V, GND, **Select** (3.3 V active high, per edge, to the panel MCU), **Comm** (24 V open-collector, active low, pulled up by controller). Pins 1, 2, 4 bussed across a tile's three edges | B |
| Nanoleaf patent (priority 2016-04-22): each side has "power, ground, bidirectional serial communication and a dedicated layout detection pin"; IDs factory-unique in NVM or assigned at init; hot-plug via polling or interrupt — matches the above | A ([US10806009B2](https://patents.google.com/patent/US10806009B2/en)) |
| Comm: 115200 8N1 on a shared open-collector bus; collision avoidance handled by protocol | B |
| Frame: `[cmd][short ID or FF=broadcast][payload][CRC16-CCITT on some]` | B |
| 8-byte long ID, 1-byte short ID assigned at enumeration | B |

Commands [B]:

| Cmd | Meaning |
|---|---|
| `01` | Assign short ID by long ID (reply `80 00 00`) |
| `02` | Set edge Select pins (bitmask 1/2/4) |
| `03` | Read long ID — only answered if a Select pin is driven by a neighbour (reply `83 ?? ?? LID×8 CRC`) |
| `08` | Fade to R G B W with 16-bit fade time |
| `09` | Call for unconfigured tiles |
| `0A` | Presence check |
| `0B` | Brightness |
| `0C` | Firmware version (ASCII + CRC) |

Topology discovery: the controller makes a known tile drive one edge's Select pin, then broadcasts `03`; only the neighbour on that edge answers.

## 4. Lines (NL59)

Nothing public about inter-segment signalling.

## 5. Synthesis and capture plan for Canvas [C]

Lineage as it appears from public data:

| | Aurora (2016) | **Canvas (2018)** | Shapes (2020) |
|---|---|---|---|
| Pins per edge | 4 | 3 | 3 |
| Supply | 24 V | 42 V | 42 V |
| Data | shared open-collector bus + per-edge select | per-side `EDGEn` nets | per-edge point-to-point, relayed |
| Logic level on data | 24 V OC | 3.3 V (probable) | 3.3 V |
| Baud | 115200 | **unknown** | 1 000 000 |
| Host SoC | RT5350 | MT7688AN | MT7688AN |
| Panel-bus chip | ? | U1 (unidentified) | EFM8BB10F8G |
| API panelId width | 8-bit | 16-bit | 16-bit |

Canvas already has the Shapes traits (3 pins, 42 V, `EDGE` naming, MT7688AN, 16-bit IDs, shared firmware version line since 9.2.0), so the first test is **1 Mbaud 8N1 with `80`/`C0`/`E0`/`F8`/`FC` opcodes**; the ~50 ms `C0` poll is the easiest thing to spot. Counterpoint: Nanoleaf states product lines use different linkers and "control logic", and Canvas's radio chip differs, so expect deviations.

Capture tips:
- Sample at ≥ 10 MHz. 2 MHz is marginal at 1 Mbaud; 500 kHz is useless.
- Tap the Control Square's 6-pad `EDGE1..4 / 3.3V / GND` header rather than the edge springs.
- Keep probes away from the ~40–42 V pin; confirm pin order with a meter first.
- Capture: cold boot with 1 panel, then 2, then hot-plug; a static colour change; brightness change; an animated scene; touch on a known panel.

## 6. Gaps — nothing public found

- Canvas: everything at protocol level (see §1).
- Shapes: panel MCU part, layout branch-return encoding, poll-reply byte meaning, `FC 07` semantics, CRC scheme.
- Elements beyond "same linkers as Shapes"; Lines entirely.
- Panton's part 2 (firmware extraction, custom controller build).
- EEVblog Aurora teardown thread (CAPTCHA-blocked) and old forum.nanoleaf.me threads (defunct / Cloudflare).
