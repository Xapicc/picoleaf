# Open questions

Grouped by relevance to the [scope](scope.md). The order of work is in the scope's work plan.

## Blocking the base scope

### Electrical
- Physical pin order of the 3-contact edge connector. Canvas silkscreen says the centre pin is ~40 V; the Shapes ESP32 project says the centre pin is GND. See [hardware.md](hardware.md#4-power-supply-and-connector).
- Voltage, idle state and pull-ups on `EDGEn`. Who drives the line when, and how direction hands over.
- Whether squares power up and hold state on the stock PSU with no controller on the bus.
- What sits behind the PSU plug's `DATA` contact, and whether the PSU takes part in the bus as on Shapes.

### Link layer
- Baud rate and framing. Hypothesis from Shapes: 1 Mbaud 8N1 single-wire half-duplex.
- Relayed hop by hop, or shared?
- Reply timing windows, and whether a PIO half-duplex UART on the Pico meets them.
- Whether a resistor-only interface (no buffer ICs) gives clean enough edges at the actual baud rate.
- Keep-alive or poll requirement: do squares blank or reset without a periodic poll (Shapes polls every ~50 ms)?
- Checksums or CRC, if any.

### Commands
- Boot and enumeration sequence; whether any handshake or version check is required before colour is accepted.
- Colour push format: channel order, W channel, transition field, ordering of squares.
- Whether the Shapes opcode set (`00`, `80`, `C0`, `E0`, `F8`, `FC`) applies at all.
- Which commands must never be sent: firmware update or reset equivalents of Shapes `FE`.

### Hardware to confirm along the way
- Role of U1, the TSSOP next to the EDGE header: panel-bus MCU or buffer?
- Where on the Control Square the bus signals can be tapped safely (the `EDGE1..4 / 3.3V / GND` 6-pad header).

## Stretch goals
- Layout encoding for squares: 4 edges, rotation, branch returns. Mapping to OpenAPI `positionData` x/y/o.
- How 16-bit panelIds are derived.
- Global brightness command; panel-side transition semantics.
- Hot-plug signalling.
- Touch report format on the wire; whether panels or the controller detect swipes.
- Maximum sustainable update rate.

## Background (out of scope, not work items)
- Panel MCU, LED, LED driver and touch IC part numbers.
- Control Square debug pinouts, boot log, OpenWrt version.
- `.firmware` image format, encryption and signing (images are archived in `firmware/canvas/`).
- Open ports on a provisioned Canvas; NPP (TCP 12566) and NLWC (TCP 3154) protocols.
- Whether Canvas was affected by CVE-2022-47758 or CVE-2026-33268.
- Post-2018 Control Square hardware revisions.
