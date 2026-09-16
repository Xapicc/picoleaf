# Open questions

What is still unknown, and what no longer is. Last revised 2026-09-16.

The base scope and the Home Assistant phase are both met ([scope.md](scope.md)), so the old grouping by "blocks the base scope" is gone: those questions are in [Answered](#answered) at the bottom. What remains either limits a stretch goal or is background.

## Still open

### Would change what the firmware can do

- **Touch report format.** Canvas squares are touch-sensitive and the OpenAPI reports per-panel touch with a type and strength, so it must cross the bus somewhere. The only candidate seen is the poll reply's first byte, which varies (`10`, `11`, `12`, `20` observed, otherwise `00`) without a known trigger. Nothing is decoded, and whether panels report swipes or the controller infers them is open.
- **Hot-plug signalling.** `C0` answers `CC` outside a session, which on Shapes means "hot-swap detected". Inside a session no such signal has been seen, so `controller.c` re-reads the layout whenever the square count in the poll reply changes. That works, but it may be polling around an explicit signal we have not recognised.
- **Panel-side transition unit.** The `TT` byte of `E0 01` is long and looks non-linear: `05` faded over a minute or more, `03` and `0A` showed no visible change within 12–20 s. It was never quantified, because the firmware sends `00` and fades on the Pico instead.
- **The W byte** of each 6-byte `E0 01` entry. It is assumed to be a white channel by analogy with the OpenAPI's extControl v2 (R G B W); its effect on a square has never actually been tested. The USB `fill`/`set` commands accept it, Home Assistant does not expose it.
- **Maximum sustainable update rate.** 25 Hz held for 10 minutes with 13 squares and no lost session. The ceiling was never probed, and how it scales with square count is unknown.

### Layout and identity

- **Side numbering direction.** The model that fits every reading so far — the header's low nibble is the side facing the parent, numbered from the square's own frame (bottom 0, left 1, top 2, right 3) — could not be tested by rotating a square in place, because the bench slots do not allow it. The 13-square blind test does confirm that the traversal is clockwise and not mirrored.
- **How the OpenAPI's 16-bit panelIds are derived.** Not needed here: entity identity uses the 16-byte hardware ID from `F8 <i> 82` instead.
- **What `E0 02` is.** It drops the session, so it is probably a longer frame format with 16-bit IDs, but nothing was decoded.
- **Whether commands carry a CRC.** Replies end with CRC-16/ARC over the bytes after the leading `01`. Colour frames were accepted both with and without a trailer, so panels appear not to require one on commands, but this was never tested systematically.

### Electrical

- **Why the supply pad measured 33 V** when the adapter is labelled 42 V ([hardware.md](hardware.md#bench-measurement-2026-09-14)). Not explained; it has no practical consequence, since that wire is cut and insulated.
- **What is behind the PSU plug's `DATA` contact.** The PSU shows up in the layout as a one-edge node (`93 00`) and ignores `F8` reads, so the bus knows about it; whether it ever drives the line is unknown.
- **Role of U1**, the TSSOP next to the `EDGE` header on the Control Square: panel-bus MCU or buffer? Unanswerable without a Control Square, which the project does not have.

### Deliberately not answered

- **What Shapes' `FE` (panel firmware update) does on Canvas.** The host tool blocks the opcode and there is no plan to find out. Same for anything resembling a reset.

## Answered

| Question | Answer | Evidence |
|---|---|---|
| Pin order of the 3-contact edge connector | Supply in the centre, as the Canvas silkscreen says; the outer pins are GND and DATA. On this linker, bottom = GND, middle = supply, top = DATA. The Shapes project's "centre = GND" does not apply | [hardware.md](hardware.md#bench-measurement-2026-09-14) |
| Voltage and idle state of `EDGEn` | 3.3 V logic, idles high once powered; floats when unpowered (mains pickup with no pull) | [panel-bus.md](panel-bus.md#bench-results-2026-09-14) |
| Do squares run on the stock PSU with no controller? | Yes — white at about half brightness, and they hold their last output when the session drops | same |
| Do squares talk unprompted? | No. Nothing in 16.5 s after power-up; they only ever answer | same |
| Baud rate and framing | 1 Mbaud 8N1, single-wire half-duplex, exactly as on Shapes | same |
| Relayed or shared? | Relayed hop by hop. A square's reply carries one leading `00` per relay hop before the `01` | [panel-bus.md](panel-bus.md#two-squares-square-b-linked-to-square-a-pico-and-psu-on-a) |
| Do the reply timings suit a PIO half-duplex UART? | Yes. First reply byte arrives ~25 µs after our stop bit; 25 Hz frames and polls held for 10 minutes | [prototype-v1.md](prototype-v1.md), `stress` run in [scope.md](scope.md#base-scope-result-2026-09-14) |
| Is a resistor-only interface clean enough? | Yes at 1 Mbaud push-pull through 330 Ω. Open-drain at 1 Mbaud needs the panel's pull-up; the RP2040's ~50 kΩ internal one is too weak | [prototype-v1.md](prototype-v1.md#step-2--flash-and-self-test-the-pico-no-square-attached) |
| Is a periodic poll required? | Yes. The session dies after ~480–500 ms without `C0`; the square keeps its last colour afterwards | [panel-bus.md](panel-bus.md#session-reads-brightness-and-colour) |
| Checksums | Reply trailers are CRC-16/ARC (poly 0x8005 reflected, init 0), little-endian, over the bytes after the leading `01` | same |
| Boot and enumeration sequence | `00` (no reply) then `80` (layout) opens a session; `80` alone is ignored and commands outside a session do nothing | same |
| Does the Shapes opcode set apply? | For `00`, `80`, `C0`, `E0 01`, `F8` and `FC 04`, yes. `E0 03` (the Shapes bulk-colour form) does nothing and `E0 02` drops the session | same |
| Colour push format | `E0 01` then one `05 TT R G B W` entry per square, **farthest square first**. A frame with the wrong number of entries is silently ignored, and `01 FF` ("no change" on Shapes) is not accepted | [panel-bus.md](panel-bus.md#two-squares-square-b-linked-to-square-a-pico-and-psu-on-a) |
| Global brightness | `FC 04 vv`, `FF` = full. Squares power up at about half and forget it when their power is cut, so it is re-sent with every colour frame | [panel-bus.md](panel-bus.md#session-reads-brightness-and-colour) |
| Layout encoding for squares | Header `C<side>` per square, other sides clockwise from the entry side, depth-first, PSU as `93 00`, `40` ending the stream and closing every open branch. Parsed blind on a 13-square wall and confirmed against the physical wall | [panel-bus.md](panel-bus.md#layout-encoding) |
| Per-square identity | `F8 <index> 82` returns a 16-byte hardware ID, `F8 <index> 81` the version (panel firmware 4.6.0). The index counts squares only, skipping the PSU node | [panel-bus.md](panel-bus.md#session-reads-brightness-and-colour) |

## Background (out of scope, not work items)

- Panel MCU, LED, LED driver and touch IC part numbers.
- Control Square debug pinouts, boot log, OpenWrt version, and where its bus signals can be tapped safely (the `EDGE1..4 / 3.3V / GND` 6-pad header).
- `.firmware` image format, encryption and signing (images are archived in `firmware/canvas/`).
- Open ports on a provisioned Canvas; NPP (TCP 12566) and NLWC (TCP 3154) protocols.
- Whether Canvas was affected by CVE-2022-47758 or CVE-2026-33268.
- Post-2018 Control Square hardware revisions.
