# Scope

## Goal

**Drive Canvas Light Squares with a Raspberry Pi Pico W (RP2040), with no Nanoleaf Control Square attached.**

No Control Square is available (as of 2026-09-14), so there is no reference to capture from. The protocol has to be found by probing Light Squares directly, starting from the reverse-engineered Shapes bus as a hypothesis. If probing stalls, getting a used Control Square to capture from is the fallback.

## Base scope — definition of done

All of the following, reproducibly from what is in this repo:

1. **Standalone.** From a cold power-up, with no Control Square attached in that power cycle, the Pico brings up a set of Light Squares powered by the stock 42 V PSU.
2. **Addressing.** The Pico can find the squares connected to it and tell them apart. Mapping them to physical x/y position is not required.
3. **Per-square colour.** Each square can be set individually to an arbitrary RGB value, and the colour appears on the intended square. The W channel is also covered if the protocol has one.
4. **Sustained updates.** Colours update at ≥ 20 Hz for 10 minutes without dropouts, blanking or resets. 20 Hz is the push rate seen on Shapes.
5. **Documented.** The docs cover the pinout, electrical interface, link layer and the subset of commands we use, with raw captures committed as evidence.

Addressing is in the base scope rather than a stretch goal because on Shapes, colour pushes are ordered by the node order that layout enumeration assigns. If Canvas works the same way, there is no per-square colour without at least minimal enumeration. See [panel-bus.md](panel-bus.md#layout-discovery).

## Stretch goals (after base scope, roughly in order)

1. **Physical layout.** Decode topology and orientation into x/y positions, like the OpenAPI's `positionData`.
2. **Global controls.** Global brightness and panel-side transitions (transition time × 100 ms).
3. **Hot-plug.** Detect squares being added or removed at runtime.
4. **Touch.** Read per-square touch state (hover, down, hold, up) and swipes.
5. **Home Assistant.** Promoted to its own scope phase; see [Home Assistant integration](#home-assistant-integration).

## Out of scope

- Re-implementing the Control Square's Wi-Fi REST API, app pairing, HomeKit, Matter or the cloud. The Home Assistant integration uses MQTT, not an emulation of Nanoleaf's API.
- Decrypting, modifying or flashing Nanoleaf firmware, whether on the Control Square, its EFR32 or the panels.
- Panel firmware updates. On Shapes these use the `FE` command; we never send it deliberately.
- The Control Square's own LEDs, buttons, microphone and light sensor.
- Other product lines (Aurora, Shapes, Elements, Lines). Their documentation is used only as reference.
- Security research on the device's network services.

The existing docs on the network API and firmware stay as background reference. They are not work items.

## Base scope result (2026-09-14)

Controller firmware 0.4.0 on a 13-square wall:

| Criterion | Result |
|---|---|
| 1. Standalone | At power-up the Pico opens the session and reads the layout by itself (`stats`: 13 squares, polling at 25 Hz) |
| 2. Addressing | Layout read and drawn correctly for 1, 2 and 13 squares, confirmed against the physical wall |
| 3. Per-square colour | `E0 01` with one entry per square, farthest first; per-square colours confirmed visually on 2 squares and as a moving rainbow on 13 |
| 4. Sustained updates | `canvasbus.py stress --minutes 10`: 600 s, 14 963 colour frames at 24.9 Hz, 14 963 good polls, 0 sessions lost, longest poll gap 40.2 ms. **PASS.** The user reported it looking fine while it ran (watched part of the run, not all 10 minutes) |
| 5. Documented | `docs/panel-bus.md`, `docs/prototype-v1.md`, captures in `captures/` |

## Home Assistant integration

Added 2026-09-14.

**Goal:** the wall appears in Home Assistant as one RGB light per square plus one light for the whole wall, controlled over the network with no computer attached to the Pico.

| Decision | Choice | Why |
|---|---|---|
| Transport | Pico W Wi-Fi → MQTT with Home Assistant MQTT discovery | Standard, no custom HA code; HA creates the entities from retained discovery messages |
| Broker | Mosquitto next to Home Assistant, on the same Raspberry Pi | No broker existed; added 2026-09-14 |
| Entities | One light per square (on/off, brightness, RGB) plus one whole-wall light | User choice |
| Entity identity | Derived from each square's 16-byte hardware ID (`F8 <i> 82`) | Re-arranging squares keeps automations working |
| Secrets | Wi-Fi and MQTT credentials entered over USB and stored in the Pico's flash; never in the repo | Coding principles: no committed secrets |

**Definition of done:** after a power cycle with only USB power, the Pico joins Wi-Fi, HA shows 14 lights under one device, and turning a square on, off, dimming or recolouring it from HA changes that square within a second; the Pico reconnects by itself after the broker or Wi-Fi restarts.

**Result (2026-09-14): met**, except that a Wi-Fi outage has not been tested (broker restarts and a power cycle on a USB supply have). Details in [home-assistant.md](home-assistant.md#verification-2026-09-14).

**Effects and transitions (added 2026-09-14, met):** eight effects on the wall light (three of them position-based), smooth transitions for every light, and a layout-rotation select. Streaming from other software and sound-reactive effects are out of scope by choice.

## Hardware constraint: Pico W only

The Raspberry Pi Pico W (RP2040, B2 chip stepping) is the only active hardware in the project. It does every job:

| Job | How the Pico does it |
|---|---|
| Logic analyser | Built into the probe firmware: a PIO edge recorder on the bus pin, 16 ns resolution, 40,000 level changes per capture. [gusmanb/logicanalyzer](https://github.com/gusmanb/logicanalyzer) (Pico W builds, up to 100 Msps) is a fallback if finer raw captures are needed |
| Bus master | Probe firmware: PIO single-wire half-duplex UART, push-pull or open-drain, same GPIO as the recorder |
| Network bridge (stretch 5) | On-board CYW43439 Wi-Fi |

Everything else is passive or not part of the circuit:

| Item | Why it can't be avoided |
|---|---|
| Computer + USB cable | Flashing, host tool, logs |
| One resistor (220–470 Ω) | Series protection between GP2 and the data contact |
| Wire and a spare Canvas linker | Connection into a square's edge |
| Stock Canvas PSU | Powers the squares |
| Multimeter | Identifying which contact carries 42 V before anything is connected to the Pico |

RP2040 facts that shape the design:
- GPIO is 3.3 V and **not 5 V tolerant**. The data line must be verified at ≤ 3.3 V before connecting.
- On the Pico W, GPIO 23, 24, 25 and 29 are used internally by the Wi-Fi chip. Usable bus pins: GPIO 0–22 and 26–28. The probe uses GP2 (header pin 4, next to GND on pin 3).
- The hardware UART (PL011, 48 MHz clock) can also hit 1 Mbaud exactly (48 MHz / 16 / 3). PIO is used instead because the line is single-wire and direction changes must be fast.

## Decisions (defaults, open to change)

| Decision | Default | Why |
|---|---|---|
| Host | Raspberry Pi Pico W (RP2040 B2), for capture and for driving. An original Pico (B1) is on hand as a spare; the probe firmware doesn't use Wi-Fi or the W-specific pins, so it should run there too (untested) | User constraint; see above |
| Line interface | GP2 → 330 Ω → DATA, no buffer ICs | Limits current on contention or a wiring fault. The Shapes ESP32 project drove the line through resistors only |
| Drive mode | Push-pull first, open-drain with pull-up second | Shapes' controller drives both levels through a tri-state buffer |
| First baud rate | 1 Mbaud, then 115200 | Shapes and Aurora rates |
| Physical connection | Wires soldered to a spare linker, entering a square's edge like the controller does | No need to modify squares; keeps the stock mechanical path |
| Panel power | Stock Canvas PSU plugged into a square's linker slot | The PSU is known to power squares with no Control Square in the path |
| Pico power | USB from the computer, sharing ground with the panel PSU through the GND contact only | Needed for flashing and the host tool |
| Test set | One square first, then 2 in a chain | Isolates root vs relayed behaviour early |

## Work plan

| Phase | Output | Exit criterion |
|---|---|---|
| 1. Pinout and levels | Multimeter-identified linker contacts, data idle voltage, behaviour of a square on PSU power alone ([prototype-v1.md](prototype-v1.md) step 1) | Pin order confirmed (resolves the GND-vs-40 V centre-pin conflict); data line confirmed ≤ 3.3 V |
| 2. Probe firmware | Pico W firmware that transmits UART frames and records edges on one GPIO; host tool that saves and decodes captures (**v1, built; self-test passed 2026-09-14**) | Hardware self-test decodes its own transmission ([prototype-v1.md](prototype-v1.md) step 2) |
| 3. Listen | Captures of a square at power-up and idle | Known whether squares talk unprompted, and the idle level |
| 4. Probe | Responses to Shapes-hypothesis frames (`00`, `80`, `C0`, `FC 04`, `E0 03`) across drive modes and baud rates (**done 2026-09-14: 1 Mbaud 8N1 push-pull, square answers layout, poll, version and ID reads**) | Any reproducible reply or visible reaction; baud rate and framing determined |
| 5. Command subset | Meaning of enumeration, poll and colour frames, worked out by varying one byte at a time within observed commands | One square set to a chosen colour on command (**met 2026-09-14 with `E0 01`; transition units, W channel and poll byte still open**) |
| 6. Chain | The same with 2 squares: relaying, addressing order | Two squares set independently (**met 2026-09-14**) |
| 7. Generate | Firmware runs enumeration and colour updates on its own, without the host tool | Base-scope definition of done met (**met 2026-09-14**, see below) |

## Risks

| Risk | Mitigation |
|---|---|
| Canvas bus differs from Shapes; there is no public Canvas data and no reference to capture | Record everything, vary one setting at a time; fall back to acquiring a used Control Square |
| **42 V sits next to the 3.3 V data contact**; a slip destroys a data line or the Pico. With the Pico on USB, the computer's USB port is also in the fault path | Identify pins before any Pico connection; soldered wires, never loose probes on edge contacts; cut and insulate the V+ wire |
| Data line is not 3.3 V logic (RP2040 is not 5 V tolerant) | Measure in phase 1 before connecting |
| Squares only respond after a handshake, periodic poll or version check we don't know | Listen at power-up first; try poll + colour sequences, not single frames |
| **Frames that have never been observed on Canvas must be sent**, so an opcode could trigger a panel firmware update or reset | Only Shapes opcodes documented as benign; `FE` blocked in the host tool; no blind fuzzing across the opcode space |
| Push-pull driving collides with a panel driving the line at the same time | 330 Ω limits the current; driver releases right after each frame; open-drain mode available |
| 16-bit panelIds come from somewhere we can't reproduce, such as a UID-derived value or controller state | Base scope only needs addressing by enumeration order; stable IDs are a stretch goal |
