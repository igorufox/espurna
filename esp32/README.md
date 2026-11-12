# ESP32 on ESPurna

Arduino-ESP32 core 3 / ESP-IDF 5 through [pioarduino](https://github.com/pioarduino/platform-espressif32) (PlatformIO
Core 6.2 or newer), Matter On/Off plugs, and a flash layout with an own safeboot image that installs updates. Code under
[`code/espurna/arch/esp32`](../code/espurna/arch/esp32), builds in
[`code/platformio_esp32.ini`](../code/platformio_esp32.ini), the safeboot in [`code/safeboot-app`](../code/safeboot-app).

## Boards

| Board | Modules | Runs | Page |
|-------|---------|------|------|
| LC-Tech ESP32 relay x2 | `matter` | the lab board: Matter with 2 controllers, migrated from the two-slot layout over Wi-Fi, updates from the WebUI | [lc-tech-relay-x2.md](lc-tech-relay-x2.md) |
| Sonoff MINIR4 "Mini Extreme" | `matter` | one in a wall | [sonoff-minir4.md](sonoff-minir4.md) |
| Sonoff NSPanel EU (gen1) | `nspanel` | one in a wall: stand-alone panel, MQTT tiles, thermostats | [nspanel/README.md](../nspanel/README.md) |

Any ESP32 with 4 MB flash runs the head with a [Tasmota template](#tasmota-templates); the head drives no GPIO until
configured, so it is safe to flash on unknown hardware.

## Image and modules

One head image for every ESP32, `espurna-esp32.bin`: the merged image (bootloader, partition table, safeboot, ESPurna)
for the first serial flash at `0x0`; `espurna-esp32.ota` is the install image (the head and every module). The features,
Matter and the NSPanel among them, are modules, the same as on every chip: the module list, installing them (the Apply),
updating the head and the recovery steps are in [docs/modules.md](../docs/modules.md). The modules lie in the tail of
app0 behind the head.

## Flash layout and safeboot

Tasmota's safeboot offsets ([`code/esp32_4mb_safeboot.csv`](../code/esp32_4mb_safeboot.csv)): bootloader, partition
table, `boot_app0`, the ESPurna safeboot in the factory partition at `0x10000` (832 KB) and app0 at `0xE0000` (2880 KB:
the head, then the module area). ESPurna cannot overwrite itself: a head update restarts into the safeboot, which takes
the head and writes app0. Safeboot reads Wi-Fi, the hostname, the admin password and `webFwUrl` from the ESPurna settings
and has none of its own; its page (`http://<board>/` while in safeboot) lists the releases and takes a file or a URL too.
Boards on the old two-slot layout are migrated over Wi-Fi from the `/safeboot` page of ESPurna. Details:
[docs/updates.md](../docs/updates.md#esp32-the-safeboot).

## First flash

```
esptool.py --chip esp32 --baud 460800 write_flash 0x0 espurna-esp32.bin
```

## Tasmota templates

Relays, buttons and LEDs are configured at runtime, so any ESP32 device can be set up from a
[Tasmota template](https://templates.blakadder.com/) without a rebuild:

1. Flash `espurna-esp32.bin` and connect to the device WebUI.
2. **Admin → Device Template**, find the device and press **Apply**, or open *Paste a Tasmota template* and paste the
   template JSON, e.g.
   `{"NAME":"MyDevice","GPIO":[0,0,0,0,0,0,0,0,0,0,0,0,224,225,0,0,0,0,0,544,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],"FLAG":0,"BASE":1}`
3. Check the preview (number of relays / buttons / LEDs found), press **Import Tasmota Template** and reboot.

Supported GPIO functions (current Tasmota numbering): `Relay1..32` / `Relay_i` (224-287), `Button1..32` with `n` / `i` /
`in` variants (32-159), `Switch1..32` / `Switch_n` (160-223, imported as buttons), `Led1..32` / `Led_i` (288-351),
`PWM1..32` / `PWM_i` (416-479, as LEDs), `LedLink` / `LedLinki` (544, 576). Template `NAME` becomes the device
description. Other functions (sensors, serial, etc.) are skipped, and the import overwrites existing relay, button and
LED settings.

## Nuances

- **Heap with Matter**: two controllers leave a few KB at the lowest point while commissioning; MQTT disconnects and
  Alexa pauses during commissioning (quiet mode), the WebUI stays connected.
- **Early setup**: code that runs before `systemCheck()` must not touch the web server (it does not exist yet); a
  migrator that did boot-looped the lab board, a wire was needed.
- **Safeboot must fall back to ESPurna**: a safeboot without its settings has no Wi-Fi; ours points the boot back to the
  app at start, so a board never stays in it.
- The `pioarduino` tool installer refuses MSYS: run the ESP32 build outside Git Bash.

## Build

```
cd code
pio run -d safeboot-app               # the safeboot first (ESP-IDF project), it goes into the merged image
pio run -c platformio_esp32.ini -e esp32   # the head, every module, the release files (docs/modules.md)
```

One env, `esp32`: the head and every module in one build; `espurna-esp32.ota` next to the image is the install image
of the WebUI. The merged image is written to `code/.pio/build_esp32/esp32/espurna-esp32.bin` by
[`scripts/esp32_merge.py`](../code/scripts/esp32_merge.py) (the release `.bin`); `firmware.bin` next to it is ESPurna
alone (an application image, what Tasmota's safeboot takes). Without the
safeboot built first ESPurna still runs, only the `safeboot` command refuses to switch.
