# ESP8266 / ESP8285 on ESPurna

The architecture ESPurna started on. This fork builds it with Arduino core 3.1.2 and NONOS SDK 3.0.5 (SDK 2.2.x crashed
inside its Wi-Fi stack on the home fleet) and replaces the 216 per-board builds with one head image: the board is
picked after flashing, from a device template or the settings, and its features are modules. Builds:
[`code/platformio_esp8266.ini`](../code/platformio_esp8266.ini).

## Image and modules

Upstream ESPurna had a build per board: 216 environments in `platformio.ini`, each with the pins and drivers of one
device compiled in. This fork has **one head image** for every ESP8266 (`espurna-esp8266.bin`) and the drivers as
modules, the same as on every chip: the head, the module list, installing them (the Apply), updating the head and the
recovery steps are in [docs/modules.md](../docs/modules.md). The pins are not in the build: the board is chosen after
flashing (*ADMIN → Device Template*, or the settings). Every driver stays idle until its pins or its port are set:
sensor pins (`dhtPin`, `dallasPin`, `ntcPin` ...), `rfbProv` / `rfbRX` (RF), `ltProvider` (light), `relayProv` (relay
MCUs). A UART driver (power meters, RF Bridge, relay MCUs) claims the port configured with `uartTx` / `uartRx`; the
serial log and terminal stop using that port then. Unknown hardware is safe to flash: no GPIO is driven until
configured.

| The board has | Modules | Boards (examples) |
|---------------|---------|-------------------|
| relays, buttons, LEDs only | none (the head) | Sonoff Basic, Mini, S20, 4CH / 4CH Pro, T1 1-3CH, Touch, Slampher, S31 Lite; Shelly 1 / 2; Moes MS-104B; Gosund WP3 / WS1; LC-Tech ESP-12F relay x2 / x4 |
| an RF receiver, the Sonoff RF Bridge | `rf` | Sonoff RF, Sonoff RF Bridge |
| a light (PWM, MY92XX) | `light` | Sonoff B1 / LED, AiLight-type bulbs, Arilux, MagicHome LED controllers, H801 / H802 |
| a power meter, a sensor | `sensors` | Sonoff Pow / Pow R2 / R3, S31, Shelly 1PM / 2.5, BlitzWolf SHP, Gosund SP111, Sonoff TH, DHT / DS18B20 shields |

A board that needs two of them (light + power meter, light + RF) takes both. Home Assistant, MQTT, Alexa, the
scheduler and the full WebUI are modules too.

## Boards run on this architecture

| Board | Chip, flash | Runs | Page |
|-------|-------------|------|------|
| Sonoff Basic | ESP8266, 1 MB | one on the bench: from ESPurna 1.15 to the generic image without a wire, two-step update from the WebUI; two more on a board build of this fork | [sonoff-basic.md](sonoff-basic.md) |
| Sonoff Mini (R1 / R2) | ESP8285, 1 MB | seven in walls, a second wall switch on a spare pin and a dummy relay mirroring another board; on a board build of this fork | [sonoff-mini.md](sonoff-mini.md) |
| Moes MS-104B, AC mains wall switches | ESP8266, 1 MB | four in walls, AC mains switch inputs; on a board build of this fork | [moes-ms-104b.md](moes-ms-104b.md) |
| HW-622 (ESP-12F, 1 relay, opto input) | ESP8266, 4 MB | the test board of the generic 4 MB image: memory work, updates from the WebUI | [hw-622.md](hw-622.md) |
| LC-Tech ESP-12F relay x4 (4 relays, AC or 7-30 V) | ESP8266, 4 MB used as 1 MB | on a board build of this fork; relays through jumpers RY1-RY4 | [esp12f-relay-x4.md](esp12f-relay-x4.md) |
| LC-Tech ESP-12F relay x2 | ESP8266, 4 MB | one in a wall, per-relay buttons in *follow* and *switch* mode; on a board build of this fork | [esp12f-relay-x2.md](esp12f-relay-x2.md) |

The other boards of [`hardware.h`](../code/espurna/config/hardware.h) (Sonoff, Shelly, BlitzWolf, ...) have device
templates in [espurna-devices](https://github.com/igorufox/espurna-devices); most of them were not run on hardware since
the move to generic images.

## Flash layout

One layout for every chip size: the 1 MB layout, the head from `0x0`, the module area behind it, the module directory
at `0xF9000` (its second copy at `0xF8000`), the settings at `0xFA000` / `0xFB000`, then the SDK's RF calibration and
Wi-Fi sectors. Boards installed with the old 4 MB layout keep their settings at the end of the 4 MB and move them once
(settings backup, the update, restore); 2 MB boards (Shelly 1 / 2 / 1PM / 2.5, Euromate V2) likewise: back up the
settings (*ADMIN*, *Backup*) before the switch. Details: [docs/modules.md](../docs/modules.md#esp8266).

## First flash

```
esptool.py --chip esp8266 --baud 460800 write_flash 0x0 espurna-esp8266.bin
```

GPIO0 to GND at power-up enters the ROM bootloader. Then the first setup (access point `ESPURNA-XXXXXX`, password
`fibonacci`, `http://192.168.4.1`), *ADMIN → Device Template*, and the modules the board needs.

Coming from a board build of ESPurna: the generic image reads the pins from the settings. Apply the board's template
before the update (it writes the values the build had, nothing changes on the running board) or right after the first
boot.

## Updates

The same as on every chip ([docs/modules.md](../docs/modules.md#updating-the-head)): the board restarts without modules,
the browser uploads the head to `/upgrade` (eboot copies it from behind the running image at the restart), then the
modules are written again. Boards on a full image of the older releases go through the migration image once, and ESPurna 1.15
boards have their own way: [docs/updates.md](../docs/updates.md#esp8266-upload-the-migration-image-for-older-1-mb-images).

## Nuances

- **Memory.** About 40 KB of heap region; the settings buffer (4 KB), Wi-Fi and the web server leave 10-20 KB. The
  image uses lwIP 'Lower Memory' (MSS 536): with the bigger lwIP buffers the WebUI came truncated and uploads dropped
  half-way. An upload of 825 KB takes 30-45 s. Measuring what each setup step takes at boot:
  `-DSETUP_HEAP_PROBE_SUPPORT=1`, terminal `HEAP.SETUP`.
- **In-wall boards on weak supplies.** A two-step update over ArduinoOTA bricked one Sonoff Mini whose power supply was
  dying (it rebooted in the middle of step 2). Before an update read `last reset reason` in `info`: anything but power on,
  terminal, web or OTA means the board is unstable, update it on the bench.
- **Serial pins for inputs.** The serial port (GPIO1 TX, GPIO3 RX) leaves a pin to a button, a relay or an LED set up on
  it (`btnGpio`, `relayGpio`, `relayBtnGpio`, `ledGpio`), e.g. a wall switch on RX of a Sonoff Mini R2; `uartTx0` /
  `uartRx0` set by hand win. *RELAYS → Serial port* shows the pins and which setting took one.
- **ESPurna 1.15 boards** (core 2.7.4) have about 10 KB free: a web upload drops half-way and the update stays 'in
  progress' until a restart. Their terminal takes commands as the WebSocket action `dbgcmd`.

## AC mains wall-switch inputs

Many in-wall relay modules (e.g. Moes MS-104B) connect the wall switch to a GPIO through an optocoupler, so while the
switch is ON the pin sees a 50/60 Hz pulse train instead of a steady level. A button marked as *AC mains switch* counts
these edges in a GPIO interrupt and is ON while at least *min pulses* edges were seen within one debounce window (100 ms
is recommended), optionally accepting only edges with the 50 or 60 Hz period (either full-wave or half-wave optocoupler
circuits). Such a button always works in `switch` mode: the position at boot is taken as the starting point (no toggle at
boot) and click / long-click actions are disabled, so a wall switch can never trigger reset or AP mode. Works on GPIO0..15
(and on any input-capable GPIO of the ESP32); on other pin types (GPIO16, expanders, analog) the button is disabled
instead, because a plain pin would read the pulses as constant presses and releases.

- **WebUI**: *Relays → Buttons and wall switches* has the global *AC frequency* (Any / 50 / 60 Hz) and *AC debug log*,
  plus *AC mains switch* and *Min pulses* per button. Changes other than the debug log need a reboot.
- **Tasmota templates** cannot express AC inputs (Tasmota uses `SwitchDebounce` ending with 9). Inputs imported from
  `Switch` codes become `switch` mode buttons toggling the relay with the same Tasmota number on every position change;
  mark them as *AC mains switch* on the RELAYS panel afterwards. The boards of this firmware in the device search (e.g.
  Moes MS-104B) already come with it.
- **Settings**: `btnFreq<N>` (AC detection on/off), `btnMinPulses<N>` (default 5), `btnAcFreq` (0 = any, 50, 60),
  `btnAcDbg` (pulse statistics in the debug log), together with `btnMode<N>=switch`, `btnPress<N>` /
  `btnRlse<N>=relay-toggle` and `btnDebDel<N>=100`.

## Build

```
cd code
pio run -c platformio_esp8266.ini -e esp8266   # the head, every module, the release files (docs/modules.md)
pio run -d migration-app                   # the migration image, code/migration-app/.pio/build/esp8266-1m-migration/firmware.bin
```

Files in `code/.pio/build_esp8266/esp8266/`: `espurna-esp8266.bin` (+ `.bin.gz`), `espurna-esp8266.ota`.

`code/scripts/boards_to_devices.py` regenerates the board entries of `../espurna-devices/devices.json` from `hardware.h`
(hand-made entries are kept).
