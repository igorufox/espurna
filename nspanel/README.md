# NSPanel (gen1 EU) on ESPurna ESP32

Firmware: ESPurna `esp32-port` branch, env `esp32-nspanel` (`-DSONOFF_NSPANEL`, module `code/espurna/nspanel*.{cpp,h}`).
The display keeps its joBr99 Lovelace UI TFT. ESPurna replaces the Tasmota + Berry + AppDaemon chain and talks to the TFT directly
(protocol: `lovelace-protocol.md`).

## Hardware mapping

| What | GPIO | Notes |
|---|---|---|
| Relay 1 / Relay 2 | 22 / 19 | active high |
| Button left / right | 14 / 27 | active low. Left: click toggles relay 1. Right: click runs `buttons[1]` from the JSON (ESPurna action `custom`) |
| Display UART2 | TX 16, RX 17 | 115200 8N1 |
| Display power | 4 | driven LOW (= on) at boot |
| Buzzer | 21 | LEDC channel 15, short beep on tile tap when `"beep": true` |
| NTC | 38 (ADC1) | 11.2 kOhm pull-up to 3.3 V, NTC 10 kOhm @ 25 C, B 3950 (Tasmota `ADCParam1 2,11200,10000,3950`, ESPHome configs use the same values) |

Any hardware button can use the JSON action: set its ESPurna action to `custom` (e.g. `btnClick0` = `custom`), then `buttons[i]` applies.

## Config file `/nspanel.json`

Stored on LittleFS in the existing data/spiffs partition (Tasmota's `spiffs`, ~320 KB). If it cannot be mounted it is formatted.
Without a file, a built-in default shows the two local relays.

Edit it at `http://<panel>/nspanel` (textarea, Save, live preview of the cards), or:

- `GET  /nspanel.json` returns the file
- `POST /nspanel.json` (body = JSON, `Content-Type: application/json`) validates, saves and reloads. 400 with the error text when invalid.
  Example: `curl --digest -u admin:<pass> -H "Content-Type: application/json" --data-binary @nspanel.json http://<panel>/nspanel.json`

All three need the normal ESPurna web authentication.

### Schema

```jsonc
{
  "version": 1,                                     // ignored, informational
  "mqtt": { "on": "1", "off": "0", "toggle": "2", "set": "/set" },
  "screensaver": {
    "timeout": 60,                                  // seconds until the card dims and the screensaver returns, 0 = never
    "brightness": 100,                              // active brightness 1..100
    "dim": 10,                                      // screensaver brightness 0..100 (0 = backlight off); must differ from brightness
    "weather": { "lat": 52.52, "lon": 13.40, "refresh": 900, "enabled": true }
  },
  "temperature": { "enabled": true, "offset": 0.0, "publish": 60, "show": true },
  "colors": { "on": 65222, "off": 17299, "unknown": 16904 },   // RGB565 decimal
  "beep": false,
  "debug": false,                                   // log every TX/RX frame
  "pages": [
    {
      "title": "Outside",                           // heading, max 40 bytes
      "type": "grid",                               // only grid is implemented
      "items": [
        { "name": "Porch", "icon": "outdoor-lamp", "topic": "ESPURNA-PORCH/relay/0" },
        { "name": "Gate", "icon": "gate", "topic": "ESPURNA-GATE/relay/0", "type": "button" },
        { "name": "Panel light", "icon": "lightbulb", "relay": 0 },
        { "name": "All lights", "icon": "lightbulb-group-off", "type": "scene", "group": "house", "action": "off" }
      ]
    }
  ],
  "groups": { "house": [ "ESPURNA-KITCHEN/relay/0", "ESPURNA-HALL/relay/1", 0 ] },
  "buttons": [ { "relay": 0 }, { "group": "outdoor", "action": "toggle" } ]
}
```

Item fields:

| Field | Meaning |
|---|---|
| `name` | tile label, max 30 bytes (Cyrillic = 2 bytes per letter, so ~15 letters) |
| `icon` | MDI name from the built-in table (`nspanel_icons.h`), or a raw TFT codepoint `"E334"` / `"U+E334"`, or a literal glyph. An unknown name shows `alert-circle-outline` and is listed in the save warnings |
| `topic` | absolute MQTT **state** topic (not under the panel's own root). Command topic = `topic + mqtt.set`, or `"command": "<topic>"` |
| `relay` | local relay index (0-based), instead of `topic` |
| `type` | `switch` (default): tap toggles. Known state → sends `on`/`off`; unknown → sends `toggle` (`"2"`, ESPurna toggle payload). `button`: tap sends `on` once (gates are pulse relays, the relay turns itself off). `scene`: `action` (`on`/`off`/`toggle`) over all members of `group`; `toggle` = any on → all off, else all on. `text`: read-only, shows the first 4 characters of the state payload in the icon slot |
| `group`, `action` | for `scene` |

Tile colour: on = `colors.on`, off = `colors.off`, unknown = `colors.unknown`. A scene is "on" when any group member is on.

`groups`: name → list of state topics (strings) or local relay indices (numbers). Members are subscribed too, so their state is known.

`buttons[i]` (hardware button i, when its ESPurna action is `custom`): `{"relay": n}` / `{"topic": "..."}` with `"action"`
(`toggle` default, `on`, `off`), `{"group": "...", "action": ...}`, or `{"page": n}` (show the first card of page n).

Pages with more tiles than one card holds are split automatically (`Title 1/2`, `Title 2/2`). Card capacity: 8 (`cardGrid2`) when
the TFT reports version >= 51, else 6 (`cardGrid`); a chunk of up to 6 tiles always uses `cardGrid`.

Navigation is linear: the screensaver sits left of the first card (its left arrow is a home icon), the arrows step through the
cards in order, the last card has no right arrow. The Lovelace TFT has no real swipe gestures (its `mSwipe*` objects are plain tap
hotspots and the Nextion page swipe attributes are disabled, also in v4.7.x).

## What the panel shows

- Screensaver: time (ESPurna NTP / local time zone), date in Russian (`Суббота, 24 октября 2026`, shortened to fit 40 bytes),
  Open-Meteo weather: current temperature + icon, 4 forecast slots (today + 3 days, `Пн 12°/5°`). With the NTC enabled and
  `temperature.show`, the second main value (alternative layout) is the room temperature. Top-left status icon: wifi-off (no WiFi)
  or alert (no MQTT).
- Tap the screensaver → first card. Idle `timeout` seconds → screensaver.
- Weather: `https://api.open-meteo.com/v1/forecast` (no key), every `refresh` seconds (retry after 60 s on error), fetched in a separate
  FreeRTOS task (TLS without certificate check), so the loop and the web server are not blocked.

## MQTT

- Subscribes to every state topic of items and groups (raw, absolute), again on every reconnect.
- Publishes commands to `<topic>/set` with `mqtt.on` / `mqtt.off` / `mqtt.toggle`, not retained.
- `<panel root>/nspanel`: every TFT event as received, e.g. `event,startup,51,eu` (shows the real TFT version), `event,buttonPress2,i3,button`.
- `<panel root>/temperature`: NTC temperature, every `temperature.publish` seconds.

## Terminal

| Command | |
|---|---|
| `nspanel.info` | TFT version/model, screen, card, frame counters, config summary and errors, FS, weather, NTC, boot partitions, last 8 events |
| `nspanel.reload` | re-read `/nspanel.json` |
| `nspanel.send <payload>` | queue a raw command, e.g. `nspanel.send pageType~pageStartup` |
| `safeboot` | set the boot partition to factory (Tasmota safeboot) and restart |

## Flash layout, install and update

### Serial flash (recommended, tested)

Flash the merged image (`espurna-esp32-nspanel.bin`, bootloader + ESPurna partition table + app) once over the J1 header
(3V3, ESP_TX, ESP_RX, GND, IO0; hold IO0 low at power-up). The display is powered from the 5 V of the relay board, so it stays
dark on a USB-UART adapter alone. Example:

`esptool.py --chip esp32 --port COMx --baud 460800 write_flash --erase-all -z 0x0 espurna-esp32-nspanel.bin`

Then configure WiFi from the ESPurna AP, set MQTT and NTP, upload `/nspanel.json`. With the ESPurna layout (two OTA slots)
later updates are normal web OTA with `firmware.bin`; `safeboot` and the crash-loop guard below do nothing.

### Tasmota safeboot layout (OTA from Tasmota, untested)

The panel keeps the **Tasmota safeboot layout**: `nvs`, `otadata`, `safeboot` (factory, ~832 KB), `app0` (ota_0, 2880 KB),
`spiffs` (~320 KB). There is no second OTA slot, so ESPurna cannot update itself in place:

- ESPurna web `/upgrade` answers 400 "...run `safeboot`..." on this layout; ArduinoOTA and the terminal `ota` client are disabled
  in the NSPanel build.
- Firmware to upload: `code/.pio/build-nspanel/esp32-nspanel/firmware.bin` (the app image only, **not** `espurna-esp32-nspanel.bin`,
  which is a merged image for serial flashing with a different partition table).

Install from Tasmota (first time):
1. In the Tasmota console, check the TFT version first: `CustomSend pageType~pageStartup`, expect `{"CustomRecv":"event,startup,51,eu"}`.
2. Tasmota web UI → Firmware Upgrade → upload `firmware.bin`. Tasmota restarts into safeboot, writes the image into `app0`, boots it.
3. ESPurna starts its AP (no WiFi credentials yet): connect, configure WiFi, MQTT, NTP time zone, password.
4. Open `http://<panel>/nspanel`, paste `nspanel.json`, Save.

Update later: terminal (telnet / web debug) `safeboot` → the panel reboots into Tasmota safeboot (it uses Tasmota's own WiFi
settings, still in NVS unless NVS was erased) → its web UI → upload the new `firmware.bin`.

Crash-loop guard: when the panel boots 3 times in a row with a crash reset (panic / watchdog, or a software restart ESPurna did not
request itself) without reaching 60 s uptime, it switches the boot partition to safeboot and restarts. The counter lives in RTC memory
and is cleared by a power-on reset. Deliberate ESPurna restarts (web, terminal, MQTT, OTA...) are not counted.

## Status

Tested on one NSPanel EU, serial-flashed with the ESPurna layout: TFT Lovelace v4.1.1 reports `event,startup,51,eu`, screensaver
(clock, Russian date, Open-Meteo weather and forecast, room temperature), 8-tile cards built from the JSON, MQTT subscriptions
and web OTA (boots from the second slot).

Not verified yet:

- TFT versions other than 51: 50/52/53 accepted, `cardGrid2` only with >= 51, `renderCurrentPage` (late 53) re-renders the card.
- The Tasmota safeboot install path, `safeboot` and the crash-loop guard.
- NTC constants are from the Tasmota template and ESPHome configs, not measured; the sensor sits near the ESP and reads high —
  use `temperature.offset`. ADC is read with `analogReadMilliVolts` (11 dB attenuation).
- Icon codepoints: taken from the 2023-06 `icons.json`; glyphs added to MDI after the TFT font was generated may render blank.
- Byte limits: names are cut at 30 bytes, headings and the date at 40, forecast values at 10 (assumed bytes, not characters).
  Long Cyrillic names (> 15 letters) are truncated.
- Frame pacing: 75 ms between frames, frames of the same kind are coalesced in the queue.
- Room temperature in the screensaver's alternative layout (6th weather group): layout and field size unverified.
- NVS is shared with Tasmota's leftovers (20 KB); ESPurna settings need ~4 KB.
