# Home Assistant integration

The Pico W runs the Canvas controller and joins Wi-Fi. It shows up in Home Assistant through MQTT discovery as the device **Nanoleaf Canvas**, with one RGB light for the whole wall (with effects), one for each square, and a layout-rotation setting. Transitions fade smoothly. Verified on 2026-09-14 on the 13-square wall.

No passwords are stored in this repository. Wi-Fi and MQTT credentials live only in the Pico's flash and in a root-readable file on the server.

## Architecture

```text
Home Assistant (container, 172.20.1.100)
        │  MQTT, user "homeassistant"
        ▼
Mosquitto (container homeassist-mosquitto-1)
   172.20.1.10     on homeassist_internalNet   ← Home Assistant
   <broker-lan-ip> on macVlanNet (LAN)         ← Pico W, user "canvas"
        ▲
        │  Wi-Fi, MQTT
Pico W (LAN, DHCP) ── GP2 → 330 Ω → square DATA ── 13 squares
```

Host: a Raspberry Pi (`<pi-host>`, Debian 13, Docker 29). The Home Assistant frontend is served by nginx.

## Entities

| Entity | Controls |
|---|---|
| `light.nanoleaf_canvas_wall` | Every square at once, plus effects. Commands apply to each square's own state, so dimming the wall keeps per-square colours unless a colour is sent too |
| `light.nanoleaf_canvas_tile_0` … `_12` | One square each: on/off, brightness, RGB |
| `select.nanoleaf_canvas_layout_rotation` | How the wall hangs relative to square 0: `0`, `90`, `180`, `270` (clockwise). Only affects effects that depend on position. Saved on the Pico. This wall: `180` |

"Tile N" uses the square's bus order at the time it was first discovered (0 = the square the Pico is plugged into; see [panel-bus.md](panel-bus.md#layout-encoding) for how order follows the layout). The **unique ID comes from each square's 16-byte hardware ID**, so moving squares around keeps the same entity, state and automations. Rename entities in Home Assistant if the numbers stop matching the wall.

Behaviour worth knowing:
- After power-up the squares keep their own default look (white, about half brightness) until Home Assistant sends the first command. The reported state starts as "on, white, brightness 128" to match.
- Brightness is applied by scaling each square's RGB; the controller sends full global brightness (`FC 04 FF`) with every colour frame, because squares power up at about half and forget `FC 04` when their power is cut.
- Squares that appear later (a re-arranged or extended wall) get the wall's last state if the wall has been commanded, otherwise the default.
- Light state is kept in RAM; after a Pico reboot everything starts from the default again. The layout rotation is stored in flash.
- **Transitions** (`"transition": seconds` in a command, or the transition option in scenes and automations) fade linearly on the Pico at 25 Hz, for the wall and single squares. The panels' own transition byte isn't used: its unit is far too long (see [panel-bus.md](panel-bus.md#session-reads-brightness-and-colour)).

## Effects

Selected on `light.nanoleaf_canvas_wall` (effect dropdown, scenes, automations). They run on the Pico at 25 Hz; nothing streams over MQTT.

| Effect | Uses the wall's colour | Moves across the wall |
|---|---|---|
| Solid | — (stops the effect; each square shows its own state) | — |
| Colour cycle | no, cycles the hue every 20 s | no |
| Breathe | yes, 4 s in and out | no |
| Twinkle | yes, dim background with random white sparkles | no |
| Rainbow wave | no | left to right, 6 s |
| Colour wave | yes | crests travel upwards, 3 s |
| Ripple | yes | rings travel outwards from the centre, 2.5 s |
| Fire | no | hottest along the bottom |

- Brightness and on/off of the wall apply to effects; changing the wall colour while Breathe, Twinkle, Colour wave or Ripple run changes their colour without restarting them.
- Controlling a single square stops the effect (the wall state reports `Solid`) so the change is visible.
- Positions come from the layout reply, parsed on the Pico (`src/layout.c`, a port of `tools/layout.py`). "Up" follows the layout-rotation setting.

## MQTT topics

`<board>` is the Pico's unique board ID in lowercase hex (`e6614c311b825937` for this unit), `<square>` a square's hardware ID in lowercase hex, and `<object>` is `wall` or `<square>`.

| Topic | Direction | Content |
|---|---|---|
| `homeassistant/light/canvas_<board>/<object>/config` | Pico → HA, retained | Discovery, JSON schema, `supported_color_modes: ["rgb"]`, unique ID `canvas_<board>_light_<object>` |
| `canvas/<board>/<object>/set` | HA → Pico | `{"state":"ON","brightness":0-255,"color":{"r":..,"g":..,"b":..},"effect":"Fire","transition":2}`, any subset; `effect` only on the wall |
| `canvas/<board>/<object>/state` | Pico → HA, retained | Same shape plus `"color_mode":"rgb"` |
| `canvas/<board>/status` | Pico → HA, retained | `online`; `offline` as the last will |
| `homeassistant/select/canvas_<board>/layout_rotation/config` | Pico → HA, retained | Select discovery, options `0 90 180 270` |
| `canvas/<board>/layout_rotation/set` / `state` | HA → Pico / Pico → HA (retained) | `0`, `90`, `180` or `270` |

## Broker setup on the server

Applied on 2026-09-14 in the Pi's existing Home Assistant Compose project). Backups: `docker-compose.yml.bak-20260914` (before the broker) and `docker-compose.yml.bak-20260914-mac` (before pinning the MAC).

Service added to `docker-compose.yml`:

```yaml
mosquitto:
  image: eclipse-mosquitto:2
  restart: unless-stopped
  volumes:
    - ./mosquitto/config:/mosquitto/config
    - ./mosquitto/data:/mosquitto/data
  networks:
    internalNet:
      ipv4_address: 172.20.1.10
    macVlanNet:
      ipv4_address: <broker-lan-ip>
      # Docker assigns a random MAC per container start; LAN clients keep the old one in their ARP cache.
      mac_address: "72:8e:15:8d:93:ed"
```

`mosquitto/config/mosquitto.conf`:

```text
listener 1883
allow_anonymous false
password_file /mosquitto/config/passwd
persistence true
persistence_location /mosquitto/data/
log_dest stdout
```

- Users `homeassistant` and `canvas`, random 48-hex-digit passwords, created with `mosquitto_passwd`. The plain-text passwords are in `~/mqtt-credentials.txt` on the Pi (mode 600).
- Started with `docker compose up -d --no-deps mosquitto`, so no other container was restarted.
- Home Assistant's MQTT integration: broker `172.20.1.10`, port `1883`, user `homeassistant`.

Show the credentials: `ssh <user>@<pi-host> cat mqtt-credentials.txt`

## Provisioning the Pico W

Build and flash as in [prototype-v1.md](prototype-v1.md#step-2--flash-and-self-test-the-pico-no-square-attached), then with the Pico on USB:

```sh
.venv/bin/python tools/canvasbus.py provision --ssid <your-ssid> --mqtt-host <broker-lan-ip> --mqtt-user canvas
```

It prompts for the Wi-Fi and MQTT passwords without echoing them (or reads `CANVAS_WIFI_PASSWORD` / `CANVAS_MQTT_PASSWORD` from the environment), sends everything hex-encoded, saves it to the last flash sector and reboots the Pico. The settings survive firmware updates, since uploads don't touch that sector. The Wi-Fi network must be 2.4 GHz.

To check from USB: `send cfg show` (passwords shown only as `set`/`unset`) and `send net`.

## Firmware behaviour

| Situation | What the Pico does |
|---|---|
| Power-up | Controller opens the session and reads hardware IDs (~0.5 s for 13 squares); Wi-Fi joins and MQTT connects within ~5 s; discovery and state are republished |
| Broker restart | Notices the disconnect, clears its ARP cache, reconnects after 5 s, republishes everything |
| Wi-Fi loss | Rejoins, then reconnects MQTT |
| After (re)connecting | Publishes the 28 discovery/state messages one every 25 ms. Sending them in one burst exhausted lwIP's memory and stalled the connection until the broker's keep-alive timeout |
| Probe commands over USB (`tx`, `cap`, …) | Pause the controller; `ctl on` resumes |

## Verification (2026-09-14)

| Check | Result |
|---|---|
| Discovery | 14 lights created under one device; entity IDs `light.nanoleaf_canvas_wall`, `light.nanoleaf_canvas_tile_0…12` |
| Control from Home Assistant | On/off, colour and brightness for a single square and the wall confirmed by the user |
| Broker recreated and restarted | LAN MAC unchanged; Pico reconnected 5 s after each, Home Assistant within 10 s; a command sent afterwards worked |
| Pico on a USB power supply, no computer | Reconnected on its own; commands from Home Assistant worked |
| Effects (firmware 0.6.0) | All eight effects, colour changes during effects and transitions confirmed by the user; Fire burns upwards with rotation `180` |
| Settings migration | Flashing 0.6.0 over 0.5.x kept Wi-Fi and MQTT settings; rotation defaulted to `0` and was set to `180` from MQTT, surviving in flash |

## Operations

- **New firmware:** flash over USB; settings stay. If entity names in discovery change, Home Assistant updates names but keeps entity IDs.
- **Re-provision:** run `provision` again.
- **Rotate the Pico's MQTT password:** `docker exec homeassist-mosquitto-1 mosquitto_passwd -b /mosquitto/config/passwd canvas <new>`, `docker restart homeassist-mosquitto-1`, update `mqtt-credentials.txt`, re-provision.
- **Remove all Canvas entities:** publish an empty retained message to each `homeassistant/light/canvas_<board>/+/config` topic while the Pico is off. Home Assistant remembers deleted entities by unique ID and restores their old entity IDs if the same unique ID reappears.
