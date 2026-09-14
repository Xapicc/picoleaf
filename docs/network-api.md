# Network interfaces — OpenAPI, streaming, events, discovery

State of public knowledge as of 2026-09-14. Evidence tags: see [README](../README.md#evidence-tags).

## TL;DR

- One local API for the whole panel family, Canvas included: **plain HTTP REST on TCP 16021**, token in the URL path. Two UDP side channels: **extControl v2 streaming into the device on UDP 60222**, and a **touch data stream** out to a client-chosen UDP port.
- Nanoleaf's current services list also names **NPP (TCP 12566)**, **HomeKit (TCP 6517)**, **NLWC (TCP 3154)** and **local firmware update (TCP 80)** for "LP devices". NPP and NLWC have no public description and no community reverse engineering.
- The official docs moved from `forum.nanoleaf.me/docs` to Confluence and contain errors that matter at byte level (touch event ID, touch bit layout, lost `<placeholders>`). Corrections below.
- The richest source of undocumented commands is DMXControl's C# [NanoleafAPI](https://github.com/patrick-dmxc/NanoleafAPI), whose tests target a real Canvas.

## Documentation locations

| Doc | URL |
|---|---|
| OpenAPI spec (current, Confluence, updated 2026-07-08) — covers "Nanoleaf Light panels and Canvas Light Squares" | https://nanoleaf.atlassian.net/wiki/spaces/nlapid/pages/2789310530/Nanoleaf+Light+Panels+Open+API+Documentation |
| List of Services used by Nanoleaf devices (updated 2026-08-28) | https://nanoleaf.atlassian.net/wiki/spaces/nlapid/pages/3410591752/List+of+Services+used+by+Nanoleaf+devices |
| Motion Appendix (plugin UUIDs) | https://nanoleaf.atlassian.net/wiki/spaces/nlapid/pages/2298052616/Motion+Appendix |
| Quick Start / Auth & Security / Change log | https://support.nanoleaf.me/hc/en-us/articles/41105798500628 · …/41108368751892 · …/38580619873300 |
| Markdown mirror of the spec | https://gist.github.com/dennishn/ae06bf574748727bcce5127394a8ba43 |
| Legacy URL (now 301) | https://forum.nanoleaf.me/docs/openapi |

Both the Confluence page and the mirror lost every `<angle-bracket placeholder>` (e.g. `TouchEventsPort: `, `GET /api/v1//events?id=1,2,4`). Rebuild from prose and implementations.

## 1. Services and ports

| Service | Transport / port | Tag |
|---|---|---|
| Nanoleaf Proprietary Protocol (NPP) | TCP 12566 | A (listed), no spec |
| OpenAPI REST + SSE | TCP 16021 (docs say don't hard-code) | A |
| HomeKit (HAP) | TCP 6517 | A |
| mDNS | UDP 5353 | A |
| SSDP | UDP 1900, 239.255.255.250 | A |
| NLWC | TCP 3154 | A (listed), no spec |
| Local firmware update | TCP 80 | A (listed) |
| extControl stream (inbound) | UDP 60222 | A |
| Touch data stream (outbound) | UDP, client-chosen port | A |
| BLE GATT | Wi-Fi provisioning only | A |

Notes:
- The services list says "LP devices" without naming Canvas; assumed to apply [C].
- Elements in setup mode serves a firmware upload page at `http://192.168.2.1/`, reportedly also reachable on the LAN IP after provisioning ([dagbdagb](https://github.com/dagbdagb/nanoleaf-firmware-upgrade-notes)) [B, Elements only].
- A developer-forum post mentions a "TCP80 /device_info REST endpoint" — uncorroborated [B].
- Old Aurora: "No open ports detected via nmap scanning" ([OpenWrt forum](https://forum.openwrt.org/t/nanoleaf-light-panels/81748)) [B, not Canvas].

A port scan of a provisioned Canvas has not been published.

## 2. Discovery

### mDNS

Service `_nanoleafapi._tcp`, port 16021 by default [A]. TXT records:

| Key | Meaning |
|---|---|
| `md` | Model, e.g. `NL29` |
| `srcvers` | Firmware version |
| `id` | Random device ID, changes on reset (resets remove API authorizations) |

A second, undocumented service `_nanoleafms._tcp` on **port 6517** was captured from a Light Panels unit ([HA #105389](https://github.com/home-assistant/core/issues/105389)) [B]:

```text
port=6517, hostname='LEDE.local.', type='_nanoleafms._tcp.local.',
properties={'sf': '1', 'sh': 'ZpF0VA==', 'pv': '1.1', 'ci': '5', 's#': '1', 'c#': '5', 'ff': '1', 'md': 'NL22', 'id': 'YY:…'}
```

The TXT keys are the HomeKit Accessory Protocol set (`ci=5` = Lightbulb) under a Nanoleaf service name, and the `LEDE.local.` hostname points at an OpenWrt/LEDE base [C]. Home Assistant subscribes to both services and lists HomeKit models `NL29, NL42, NL47, NL48, NL52, NL59, NL69, NL81` ([HA integration](https://github.com/home-assistant/core/tree/dev/homeassistant/components/nanoleaf)) [B].

Device name is the factory "model + last 2 MAC bytes" (e.g. `Canvas 4A38`) and is not renamable over the API because it sets the mDNS instance name [B].

### SSDP

[A] Multicast 239.255.255.250:1900. `NOTIFY * HTTP/1.1` every ~1 min with `NT: Nanoleaf_aurora:light / nanoleaf:nl29`, `NTS: ssdp:alive`, `USN: uuid:…`, `Location: http://<ip>:16021`, `Cache-Control: max-age = 60`, `nl-deviceid`, `nl-devicename`. M-SEARCH ST `ssdp:all` or `nanoleaf_aurora:light`; unicast reply delayed randomly up to MX seconds. `NTS: ssdp:byebye` on leave.

Real Canvas M-SEARCH reply (client sent `ST: nanoleaf:nl29`; redacted by poster, [nanoleafapi #4](https://github.com/MylesMor/nanoleafapi/issues/4)) [B]:

```text
HTTP/1.1 200 OK
S: uuid:c588db09-a654-4dba-b60e-68de39****25
Ext:
Cache-Control: no-cache="Ext", max-age = 60
ST: nanoleaf:nl29
USN: uuid:c588db09-a654-4dba-****-68de39749a25
Location: http://192.168.86.64:16021
nl-deviceid: 4D:EC:F1:**:BC:0A
nl-devicename: Canvas 4A38
```

Quirks ([ioBroker changelog](https://github.com/daniel-2k/ioBroker.nanoleaf-lightpanels)) [B]:
- Canvas/Shapes firmware sent an **empty `Location`**; use the packet source IP.
- Canvas may not answer `ST: nanoleaf_aurora:light`; use `ssdp:all` or `nanoleaf:nl29`.
- `ssdp:alive` can stop; poll instead.
- `ssdp:all` also returns Philips Hue records.

## 3. Authentication

[A] Hold the power button 5–7 s until the LEDs flash (Canvas manual says 3 s), then within 30 s:

```text
POST   /api/v1/new        -> 200 {"auth_token": "…"}   errors: 401, 403, 422
DELETE /api/v1/<token>    -> 204                        errors: 401, 500
```

- "Tokens last until the device is reset"; multiple tokens allowed; no list endpoint, expiry or documented limit [A].
- Tokens are 32 alphanumeric characters (Nanoleaf SDK checks `len == 32`) [B].
- 403 on `/new` = pairing window not open [B].
- `POST /api/v1/` without token → 401 (or 204); used as a ping by DMXControl [B].
- Unauthenticated requests return `401` with `Content-Length: 0`. Captured `PUT /api/v1/<token>/state` requests replay successfully — plain HTTP, no nonce ([REPLIOT dataset](https://github.com/SafeNetIoT/ReplayAttack), Shapes 7.1.6) [A].

## 4. REST API

Base: `http://<ip>:16021/api/v1/<token>/`. "API endpoint follows the hierarchical structure of the internal JSON": GET any leaf, PUT on the parent object [A].

### Device info — `GET /`

[A] Canvas example (firmware 1.1.0): `name`, `serialNo`, `manufacturer`, `firmwareVersion`, `model: "NL29"`, `discovery {}`, `effects {effectsList, select}`, `panelLayout {globalOrientation, layout {numPanels, sideLength, positionData[]}}`, `state {brightness, colorMode, ct{min:1200,max:6500}, hue, on, sat}`.

Extra keys seen in the wild [B]: `hardwareVersion` ("2.0-4" on a Canvas 7.1.0; "1.4-0" on Shapes), `firmwareUpgrade {}`, `schedules {}`, and an unexplained `qkihnokomhartlnp {}` (Shapes 7.1.6 raw capture).

### State

| Read | Write `PUT state` | Range |
|---|---|---|
| `state/on` | `{"on":{"value":true}}` | bool |
| `state/brightness` | `{"brightness":{"value":100,"duration":30}}` or `{"brightness":{"increment":-10}}` | 0–100, duration in s |
| `state/hue` | `{"hue":{"value":120}}` / `increment` | 0–360 |
| `state/sat` | `{"sat":{"value":20}}` / `increment` | 0–100 |
| `state/ct` | `{"ct":{"value":3000}}` / `increment` | 1200–6500 on Canvas |
| `state/colorMode` | read-only | `effect`, `hs`, `ct` |

Quirks [B]: Canvas returns **400 for a bare `{"on": false}`** ([software-2/nanoleaf #16](https://github.com/software-2/nanoleaf/issues/16)). aionanoleaf puts `on` last when combining keys ("'on' must be the last key"), no reason given.

### Effects

[A] `GET effects/select`, `PUT effects {"select":"<name>"}`, `GET effects/effectsList`, `PUT effects {"write":{…}}`.

Write commands: `add`, `delete`, `request`, `requestAll`, `rename`, `display`, `displayTemp`, `requestPlugins`. Some doc examples omit the `write` wrapper; every client uses it.

Fields: `command`, `version` (`"1.0"`, plugins `"2.0"`), `duration`, `animName`, `newName`, `animType` (random, flow, wheel, fade, highlight, custom, static, plugin, extControl, solid), `animData`, `colorType: "HSB"`, `palette[{hue,saturation,brightness,probability}]`, `brightnessRange`, `transTime`, `delayTime`, `flowFactor`, `explodeFactor`, `windowSize`, `direction`, `loop`, `pluginUuid`, `pluginType` (color/rhythm), `pluginOptions[{name,value}]`.

Reserved names: `*Static*`, `*Dynamic*`, `*Solid*`; `*ExtControl*` appears while streaming [A/B].

Legacy plugin UUIDs [A]:

| Plugin | UUID |
|---|---|
| Wheel | `6970681a-20b5-4c5e-8813-bdaebc4ee4fa` |
| Flow | `027842e4-e1d6-4a4c-a731-be74a1ebd4cf` |
| Explode | `713518c1-d560-47db-8991-de780af71d1e` |
| Fade | `b3fd723a-aae8-4c99-bf2b-087159e0ef53` |
| Random | `ba632d3e-9c2b-4413-a965-510c839b3f71` |
| Highlight | `70b7c636-6bf8-491f-89c1-f4103508d642` |

**`animData` for custom/static** [A], space-separated on the wire:

```text
numPanels; panelId0; numFrames0; RGBWT01; … RGBWT0n; panelId1; numFrames1; RGBWT11; … panelIdN; numFramesN; RGBWTN1; …
```

Each frame is `R G B W T`: W "currently ignored"; transition to the colour over `T × 100 ms`; `T = -1` marks a start frame (first frame only). Panel IDs are decimal (16-bit on Canvas). Example:

```json
{ "command": "display", "animType": "static",
  "animData": "3 82 1 255 0 255 0 20 60 1 0 255 255 0 20 118 1 0 0 0 0 20",
  "loop": false, "palette": [], "colorType": "HSB" }
```

Custom animData on Canvas arrived in firmware 1.2.0. openHAB reads `*Static*` back via `write {command:"request", animName:"*Static*"}` [B]. Screen-mirror commands (2024) are for Skylight/4D only.

### Panel layout

[A] `GET panelLayout/layout` → `{numPanels, sideLength, positionData:[{panelId,x,y,o,shapeType}]}`; `GET panelLayout/globalOrientation` → `{value,min:0,max:360}`; `PUT panelLayout {"globalOrientation":{"value":120}}`.

- `x`,`y` = centroid; `o` = orientation, counter-clockwise; layout is not rotated by globalOrientation (client applies it).
- For Canvas `numPanels` **includes the Control Square**, which appears in `positionData`.
- `sideLength` deprecated and set to 0 since 5.0.0.

`shapeType` values (Canvas rows in bold) [A]:

| Name | Value | Side length |
|---|---|---|
| Triangle (Light Panels) | 0 | 150 |
| Rhythm | 1 | — |
| **Square** | **2** | **100** |
| **Control Square Master** | **3** | **100** |
| **Control Square Passive** | **4** | **100** |
| Power supply (undocumented, per Hyperion/DMXControl) | 5 | — |
| Hexagon (Shapes) | 7 | 67 |
| Triangle (Shapes) | 8 | 134 |
| Mini Triangle (Shapes) | 9 | 67 |
| Shapes Controller | 12 | — |
| Elements Hexagon | 14 | 134 |
| Elements Hexagon – Corner | 15 | 33.5 / 58 |
| Lines Connector | 16 | 11 |
| Light Lines | 17 | 154 |
| Light Lines – Single Zone | 18 | 77 |
| Controller Cap | 19 | 11 |
| Power Connector | 20 | 11 |
| 4D Lightstrip | 29 | 50 |
| Skylight Panel / Controller Primary / Passive | 30 / 31 / 32 | 180 |

The 1.1.0 doc example shows every Canvas square as 2; a 7.1.0 Canvas fixture in DMXControl has one 3 and eight 2s [B]. Passive mode (shapeType 4, extra Control Squares) arrived with Canvas 1.4.0 [C].

### Identify, rhythm

- `PUT identify` → 204, panels flash [A].
- `rhythm/*` endpoints exist for Light Panels. The doc says Canvas rhythm "is built into the control square" and the Canvas JSON has no `rhythm` key → probably absent on Canvas [C].

## 5. Streaming — extControl v2

### Enable

[A] Canvas supports **v2 only** and requires the version key ("If the extControlVersion key is omitted, v1 is assumed, which would return an error for Canvas"):

```json
PUT /api/v1/<token>/effects
{"write": {"command": "display", "animType": "extControl", "extControlVersion": "v2"}}
```

Canvas replies 204 with no body; target is the controller IP, **UDP 60222**. (Light Panels v1 returns `{"streamControlIpAddr","streamControlPort":60222,"streamControlProtocol":"udp"}`.) `effects/select` then reads `*ExtControl*`. No documented timeout; leave by selecting another effect.

### Datagram

[A] "the size of the nPanels, panelId and transitionTime fields have been increased from 1B to 2B. The nFrames field has been dropped"; "all the multi-byte fields are represented in Big Endian format".

```text
offset  size  field
0       2     nPanels            (uint16 BE)
repeat nPanels (8 bytes each):
+0      2     panelId            (uint16 BE)
+2      1     R
+3      1     G
+4      1     B
+5      1     W                  ("currently ignored")
+6      2     transitionTime     (uint16 BE, × 100 ms)
```

Official example — panel 374 → (255,0,255), T=12; panel 651 → (255,255,0), T=128; panel 235 → (0,255,255), T=451:

```text
00 03  01 76 FF 00 FF 00 00 0C  02 8B FF FF 00 00 00 80  00 EB 00 FF FF 00 01 C3
```

(The doc writes `0xC` for `0x0C`.)

- "each frame need not include all panels"; a new update mid-transition restarts from the current intermediate colour [A].
- "100ms transitions are handled at a panel level" → interpolation runs in panel firmware [A/C].
- Rate: "must not stream data at a rate higher than 10Hz" [A]. In practice DMXControl streams 10–60 Hz (default 44); the Java lib notes ~50 ms minimum spacing on Shapes. Shapes-only firmware 6.5.1 allowed updates faster than 30 ms. **No Canvas latency/rate measurement published.**
- Transition values used by clients: 0 (OpenRGB, DMXControl), 1 (Hyperion, Winleafs), 2 (audioleaf). OpenRGB streams to the Control Square's panelId too [B].

v1 (Light Panels, for reference): `nPanels` (1B), then per panel `panelId nFrames(=1) R G B W T`, all 1 byte.

## 6. Events

### Server-Sent Events

[A] `GET /api/v1/<token>/events?id=1,2,3,4` → `text/event-stream`. Canvas needs firmware ≥ 1.1.0 (doc says "> 1.10", a typo). Each message is `id: <eventType>` + `data: <json>` — the `id` is the event type, not an SSE resume ID.

| Type | ID | Payload |
|---|---|---|
| State | 1 | `attr` 1 on, 2 brightness, 3 hue, 4 sat, 5 ct, 6 colorMode |
| Layout | 2 | `attr` 1 layout, 2 globalOrientation |
| Effects | 3 | `attr` 1 selected effect (community: `attr` 2 = effectsList update) |
| Touch | 4 | `{gesture, panelId}` |

The doc also says "touch (id=3)" in one place — it is **4**.

| Gesture | ID | panelId |
|---|---|---|
| Single tap | 0 | panel |
| Double tap | 1 | panel |
| Swipe up / down / left / right | 2 / 3 / 4 / 5 | -1 |

Wire example ([rjbs](https://rjbs.cloud/blog/2023/09/nanoleaf-touch-failure/)) [B]:

```text
id: 1
data: {"events":[{"attr":1,"value":false}]}

id: 4
data: {"events":[{"panelId":-1,"gesture":5}]}
```

Reliability [B]: ~700 ms latency on state events and missing taps on Shapes (rjbs); SSE touch latency "about 1 to 2 seconds" (Java lib); built-in gesture actions (e.g. double-tap power) swallow taps unless disabled in the app (openHAB). SSE keep-alives were added for Shapes only (9.4.0) — expect idle Canvas SSE connections to time out and need reconnecting [C].

### UDP touch data stream

[A] Firmware ≥ 1.4.0. Add header `TouchEventsPort: <udp port>` to the SSE request that subscribes to id 4. Datagrams go to the requesting client's IP for as long as the SSE session lives.

Touch types: 0 Hover, 1 Down, 2 Hold, 3 Up, 4 Swipe.

The type/strength bit layout is contradictory in the official doc (prose: 3-bit type + 4-bit strength; table after a 2026-07-08 staff edit: 4-bit type + 3-bit strength). All community parsers and rjbs's empirical Shapes captures agree on **bits 6..4 = type, bits 3..0 = strength** [B/C]:

```text
offset  size  field
0       2     nPanels (uint16 BE)
repeat nPanels (5 bytes each):
+0      2     panelId (uint16 BE)
+2      1     bit7 reserved | bits6..4 touchType | bits3..0 touchStrength
+3      2     swipedFromPanelId (uint16 BE); 0xFFFF when not a swipe (doc says 0xFF)
```

Reliability [B, Shapes]: taps only arrived as hovers; delivery inconsistent; at one point two HTTP connections were needed before UDP started (rjbs). DMXControl synthesises an Up after 500 ms of silence following a Hover. Not independently confirmed on Canvas.

Library bugs [B]: aionanoleaf/aionanoleaf2 only parse the first panel and their "no swipe" check `== 2 ^ 16` is Python XOR (18), so it never matches.

## 7. Undocumented commands (community)

Unless noted, the only source is DMXControl [`Communication.cs`](https://github.com/patrick-dmxc/NanoleafAPI/blob/main/NanoleafAPI/API/Communication.cs) with its [JSON models](https://github.com/patrick-dmxc/NanoleafAPI/tree/main/NanoleafAPI/API/JSON-Objects); its tests target a Canvas NL29. Not verified on hardware. **Firmware, schedule, touch and button commands change device state.** [B]

| Method / path | Body | Response / notes |
|---|---|---|
| `GET firmwareUpgrade` | — | `{"firmwareAvailability": bool, "newFirmwareVersion": string}` |
| `PUT firmwareUpgrade` | `{"command":"triggerFirmwareUpgrade"}` | 204; starts a cloud update |
| `GET schedules` | — | `{"schedules":[{id, set_id, enabled, …}]}` |
| `PUT schedules` | `{"command":"addSchedules","schedules":[…]}` / `{"command":"removeSchedules","schedules":[{"id":…}]}` | 204. Java lib has a commented-out variant under `effects/write` |
| `PUT effects` | `{"write":{"command":"requestTouchConfig"}}` | `{"touchConfig":{"supportedFeatures":{…},"defaultSystemConfig":{…},"userSystemConfig":{"enabled","gestureActions":[…],"subscribers":[…]},"userPanelConfigs":[…]}}` |
| `PUT effects` | `{"write":{"command":"configureTouch","touchConfig":{"userSystemConfig":{…}}}}` | 204 |
| `PUT effects` | `{"write":{"command":"getTouchKillSwitch"}}` | `{"touchKillSwitchOn": bool}` |
| `PUT effects` | `{"write":{"command":"setTouchKillSwitch","touchKillSwitchOn":bool}}` | 204 (matches Canvas 5.0.1 "disable Touch" note) |
| `PUT effects` | `{"write":{"command":"enableAllControllerButtons"}}` / `disableAllControllerButtons` | 204 |
| `PUT effects` | `{"write":{"command":"enableSceneChangeAnimation"}}` / `disableSceneChangeAnimation` | 204 |
| `PUT effects` | `{"write":{"command":"requestBrightnessSensorConfig"}}` | `{"brightnessSensorConfig":{"enabled","brightnessSensorMode":0\|1,"userMaxBrightness","userMinBrightness","isCalibrated","isCalibrating"}}` |
| `PUT effects` | `{"write":{"command":"setBrightnessSensorConfig","brightnessSensorConfig":{…}}}` | 204 |
| `POST http://<ip>:6517/identify-android` | — | 200/204; HomeKit port, no token |

Gesture encodings in touch config: `st`, `dt`, `su`, `sd`, `sl`, `sr`. Actions: `pwr`, `bu`, `bd`, `ncs`, `nrs`, `nrdms`, `pcs`, `prs` (power, brightness up/down, next colour/rhythm/random scene, previous colour/rhythm scene).

Mentioned in release notes, command name not public: temporary ripple effect (6.5.1), brightness-sensor calibration reset (Shapes 4.0.2), touch parameter tuning (Shapes 5.0.0).

Not found anywhere: network factory reset, Wi-Fi config over OpenAPI, debug/shell endpoints, NPP (12566) or NLWC (3154) message formats.

## 8. HomeKit, Thread, cloud

- HomeKit on TCP 6517 with an HAP-style `_nanoleafms._tcp` record [A/B]. Canvas 1.4.0 added Light Squares as HomeKit buttons. `nl-devicename` can be set via Apple WAC [A]. Presumably standard HAP-over-IP (SRP pairing, ChaCha20-Poly1305 sessions), pairable by e.g. HA `homekit_controller` after removal from Apple Home — not verified [C].
- Thread/Matter: see [firmware-and-security.md](firmware-and-security.md#thread--matter). Canvas's radio is BLE-only; it is not a Thread Border Router despite one shared release-note line.
- Canvas 6.2.1 made Control Squares "Cloud Gateways for Nanoleaf Essentials" [A]. Cloud protocol not public; CVE-2022-47758 writeup describes an MQTT `cloud_daemon` on a sibling product.

## 9. Open-source clients

| Project | Lang | Worth reading for | Caveats |
|---|---|---|---|
| [DMXControl NanoleafAPI](https://github.com/patrick-dmxc/NanoleafAPI) | C# | Undocumented commands; SSE + `TouchEventsPort`; multi-panel touch parser; v2 streaming; Canvas tests | Sole source for most §7 items |
| [amybytes/rowak nanoleaf-api](https://github.com/amybytes/nanoleaf-api) | Java | Correct UDP touch parser (`0x70`/`0x0F`, 0xFFFF); rate notes | Last commit 2022 |
| [aionanoleaf](https://github.com/milanmeu/aionanoleaf) / [aionanoleaf2](https://github.com/loebi-ch/aionanoleaf2) | Python | SSE parsing, event classes | UDP parser bugs (above) |
| [Home Assistant `nanoleaf`](https://github.com/home-assistant/core/tree/dev/homeassistant/components/nanoleaf) | Python | Discovery constants; swipe triggers for NL29/NL42/NL52 | IPv6 URL bug ([#160461](https://github.com/home-assistant/core/issues/160461)) |
| [nanoleafapi](https://github.com/MylesMor/nanoleafapi) | Python | SSDP `nanoleaf:nl29`; v2 streaming; custom animData | Test skips panelId 0 |
| [openHAB binding](https://github.com/openhab/openhab-addons/tree/main/bundles/org.openhab.binding.nanoleaf) | Java | SSE touch handling; `*Static*` parsing; IPv4-only on ≥ 8.5.2 | SSE only |
| [ioBroker adapter](https://github.com/daniel-2k/ioBroker.nanoleaf-lightpanels) | JS | SSDP firmware workarounds | Canvas commands silently failing ([#212](https://github.com/daniel-2k/ioBroker.nanoleaf-lightpanels/issues/212)) |
| [OpenRGB](https://gitlab.com/CalcProgrammer1/OpenRGB/-/blob/master/Controllers/NanoleafController/NanoleafController.cpp) | C++ | Commented v1/v2 layouts; NL29/NL42/NL59 → v2 | Transition always 0 |
| [Hyperion.ng](https://github.com/hyperion-project/hyperion.ng/blob/master/libsrc/leddevice/dev_net/LedDeviceNanoleaf.cpp) | C++ | shapeType enum incl. 5; LED-less shapes; orientation handling | — |
| [Winleafs](https://github.com/winleafs/Winleafs) | C# | v1/v2 builders | Inactive since 2022 |
| [audioleaf](https://crates.io/crates/audioleaf) | Rust | Canvas-only visualiser, v2 streaming | GitHub repo gone; crate source remains |
| [roderm/go-nanoleaf](https://github.com/roderm/go-nanoleaf), [dsymonds/nanoleaf](https://github.com/dsymonds/nanoleaf) | Go | v1/v2 selection; mDNS; retries because requests "regularly fail" | — |
| [nanoleaf/aurora-sdk-mac](https://github.com/nanoleaf/aurora-sdk-mac) | C++/Py | Official on-device plugin SDK + simulator | Light Panels only; 2018; not for Canvas |

## 10. Known quirks (summary)

1. Doc errors: touch event ID 3 vs 4; `0xC`; contradictory touch bit table; lost placeholders; v1 example mismatch; "> 1.10"; `Palette` capitalised.
2. Canvas rejects extControl without `extControlVersion: "v2"`.
3. SSDP: empty `Location`, ST mismatch, `ssdp:alive` stops.
4. SSE touch is slow/lossy; built-in gestures swallow taps.
5. UDP touch stream unreliable (observed on Shapes).
6. Intermittent HTTP failures / connection resets; clients add retries ([aionanoleaf PR #7](https://github.com/milanmeu/aionanoleaf/pull/7), [HA #105389](https://github.com/home-assistant/core/issues/105389)).
7. IPv6 addresses advertised; clients that don't bracket them break.
8. Device name not renamable via API.
9. Canvas is strict about state bodies (`{"on": false}` → 400).
