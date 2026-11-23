# Modules: a small head image and the parts you switch on

**Status: prototype.** One loader for ESP8266, ESP32 and Beken (`code/espurna/modules.cpp`, its interface
`modules_core.h`); a chip adds only what its hardware forces ([Per chip](#per-chip-what-the-hardware-forces)). One build
per chip makes the head, every module and the release files; CI (`.github/workflows/esp32.yml`) builds the three and
publishes them ([docs/updates.md](updates.md)).

## What it is

Every board runs the **head**, one image per chip with what every board needs: Wi-Fi, relays, buttons, LEDs, the web
server with a small page of its own, the API, the terminal and telnet, NTP, OTA, the module loader, and the forwarders
of the MQTT and WebSocket APIs (relays, buttons ... call MQTT and the WebUI as always, the `mqtt` and `ws` modules take
over when they are on). Everything else
is a **module**: a separate piece of code, or data such as the WebUI, that the board writes into the free flash of its
module area and starts at boot. You pick the modules on the board's MODULES page; nothing has to be built for a board.

What it gives:

- **one image per chip** instead of an image per feature set, and boards that needed two feature sets get both;
- **head updates on small flash**: the head is small enough for its update to fit where the modules were;
- **a board that always answers**: without modules, with modules of another build, or after a module crashed it, the
  board runs the head: relays, buttons, the API, telnet and the head's own page keep working.

## Modules

One list for every chip ([`code/espurna/config/head.h`](../code/espurna/config/head.h), `ESPURNA_MODULES`); a chip
leaves out only what cannot exist on it.

| Module | What it brings | Chips |
|--------|----------------|-------|
| `webui` | the full WebUI (data, no code: `code/html/build/index.html.gz` of `npx gulp build`); without it the head shows its own page; needs `ws` | all |
| `ws` | the WebSocket of the WebUI (`/ws`, `/auth`): the live state, settings, actions, the debug log; the head keeps the callbacks and forwards `wsPost`/`wsSend` to it (`modules_ws.h`) | all |
| `captive` | the captive portal of the board's access point; **on out of the box** | all |
| `mqtt` | MQTT | all |
| `homeassistant` | Home Assistant MQTT discovery (needs `mqtt`) | all |
| `domoticz` | Domoticz (needs `mqtt`) | all |
| `thingspeak` | Thingspeak | all |
| `sensors` | the sensor manager and its drivers (power meters, DHT, Dallas, BMx280, analog, NTC, digital ...) | all |
| `light` | lights: PWM, my92xx | all |
| `rf` | 433 MHz receiver (rc-switch) | all |
| `scheduler` | the scheduler | all |
| `alexa` | Alexa (local, FauxmoESP) | all |
| `mdns` | mDNS (`<hostname>.local`); not next to `matter`, whose stack answers mDNS itself | all |
| `matter` | Matter On/Off plugs | ESP32, Beken |
| `screen` | what every screen has in common, one implementation for every panel (screen.h): the drawing in its own fonts (DejaVu, Latin-1 and Cyrillic, `scripts/screen_fonts.py`) and icons (Material Design Icons of the config), up to 8 lines of a label and a source (time, IP, a relay, a sensor, an MQTT topic ...) in pages (`scrLbl<n>` / `scrSrc<n>`, `scrFont`, `scrPage`; the oled module's `oled*` lines become these once), the Matter pairing QR code while its window is open (`scrMatter`, `SCREEN.QR`), blanking and brightness (`scrOff`, `scrBright`; the button action `display-on`), the keys (`SCREEN.KEY prev / next / enter / back`: a button with a terminal command action), the WebUI mirror (SCREEN panel); and the tile pages of `/nspanel.json` (PAGES panel: entities on MQTT topics and relays, scenes, thermostats, the weather), shown by the nextion module, or drawn here on a pixel panel (the M5Stack). Its panels are modules of their own: `oled`, `lcd`, `nextion` ([docs/screen.md](screen.md)) (needs `tls`, `http`) | all |
| `oled` | a screen panel: the small I2C screen (SSD1306 / SH1106), the controller, size, address and the reset pin in the settings (`oledType`, `oledSize`, `oledAddr`, `oledRst`, the bus is `i2cSDA` / `i2cSCL`), the `OLED` command (its pixels) (needs `screen`, `i2c`) | all |
| `lcd` | a screen panel: a colour TFT on SPI, ILI9341 / ILI9342 / ST7789 (`lcdType`, `lcdSize`, `lcdRot`, `lcdBgr`, `lcdInv`, `lcdCS`, `lcdDC`, `lcdRst`, `lcdBL`, `lcdMHz`; the bus is `spiSCK` / `spiMOSI` / `spiMISO`). The M5Stack Basic: `ili9342`, CS 14, DC 27, reset 33, backlight 32, its buttons A / B / C on GPIO 39 / 38 / 37 as `screen.key prev / enter / next` (needs `screen`, `spi`) | all |
| `nextion` | a screen panel with pages of its own: a Nextion display on a UART port (the NSPanel's: `nspPort`, `nspPower`), the OHpanel and Lovelace UI drivers detected automatically, the TFT upload (`NSPANEL.FLASH`, NEXTION panel); it shows the tile pages of the screen module, the screen draws on it only its QR page (needs `screen`, `uart`, `tls`, `http`) | all |
| `test` | a tiny module for testing the loader | all |
| `arduino_emu` | the emulation of the Arduino names (`String`, `Print`, `pinMode`, `Wire`, `SPI`, `Serial`, `WiFiUDP`: `code/lib/espurna-core`) for the third-party libraries of the modules: only modules whose library needs it have it in their needs (the sensors on a library's Wire, IR, RF, RFM69, MCP23S08, Alexa, RPN rules, garland, my92xx); linked into no other module. The buses (`i2c`, `spi`) and the screen panels (`oled`, `lcd`) are on the chip calls (`arch/rtos/chip_sdk.h`), without it. Beken: empty, LibreTiny's core is in the head | all |

A module that needs another one (`requires`) cannot be switched on without it, and the other one cannot be switched off
or removed while it is needed. Modules that use each other's functions do it through the head (Home Assistant shows the
sensors when `sensors` is on, the scheduler drives the lights when `light` is on); without the other module they simply
have less to show.

The limit is RAM, not flash. Every module's variables (`.data` + `.bss`) need room, and so do the allocations it makes
when it starts; the MODULES page shows both against what the board has. Switch on what the board needs.

## Release files

| Chip | Initial image (first flash by wire, and OTA of the head) | Install image (head + every module) |
|------|-----------------------------------------------------------|-------------------------------------|
| ESP8266 | `espurna-esp8266.bin` (serial at `0x0`; the WebUI takes its `.bin.gz` from GitHub Pages) | `espurna-esp8266.ota` |
| ESP32 | `espurna-esp32.bin`, the merged image (serial at `0x0`) | `espurna-esp32.ota` |
| Beken | `espurna-beken-tuya.uf2`, `espurna-beken-quectel.uf2` (one per bootloader) | `espurna-beken.ota` (both heads) |

- **The initial image** is the head and, at the start of the module area, a modules file of the modules that are on out
  of the box (`MODULES_ON_BY_DEFAULT` of `head.h`: `captive`). At its first boot a head with no directory of its own
  installs them with the same Apply as below, erases the file and restarts.
- **The install image** (`.ota`, [`scripts/module_ota.py`](../code/scripts/module_ota.py)) holds the head (on the Beken
  one per bootloader) and the modules file (`EMC2`, every module package of the build). It is not for a serial flash
  tool: the WebUI takes it apart.

A module belongs to the head build it was linked against: a module of another build is not started (the page shows it
as *stale*). The install image always carries the head and the modules of the same build.

## Installing modules: Apply

**MODULES** in the WebUI menu; without the `webui` module the head answers on `/` with its own page (`/modules.html`,
`modules_page.h`), which does the same.

1. **Source.** The `.ota` of a release, from the disk or *From GitHub* (the release the board runs).
2. **Tick** the modules. The page shows what each takes of the flash and the RAM, and the meters what is left.
3. **Apply.** The WebUI ([`code/html/src/install.mjs`](../code/html/src/install.mjs), `installImage`) does:
   1. when the board runs another head build than the modules of the file: the head first ([below](#updating-the-head));
   2. `POST /modules/off`: the off mark in the directory, a restart without module code;
   3. `POST /modules/apply` with the ticked names and the modules file: the board writes the ticked packages one after
      another into the area from the top down, relocating each as it arrives, builds the new directory in memory and
      writes it once at the end, then restarts;
   4. the modules start at the next boot.

The Apply plans the RAM of every module anew each time, no reserve kept: first the chip's fixed pool (ESP8266: the IRAM
after the head's code; Beken: the free data TCM; ESP32: none), largest module first, then one heap block that the next
boot takes before the modules start.

An Apply that breaks off (power cut, the page closed) leaves the old directory with the off mark: the board comes up
with the network and without modules, the page says why. Apply again; *Start the modules again* (`MODULES.ON`) brings
the old ones back when their packages were not overwritten (a package that was is refused by its CRC).

Without an Apply: `MODULES.ENABLE` / `MODULES.DISABLE` (or `POST /modules/enable`) mark an installed module on or off,
in effect at the next restart; `MODULES.REMOVE` takes out a module that is off.

The firmware panel (*ADMIN*, OTA, the release list, *Install*) runs the same `installImage` with the `.ota` of the
release and keeps the modules the board has on.

## Updating the head

While module code runs, an update is refused (`409 modules installed, MODULES.OFF first`): it would overwrite what the
board runs. So the head is always updated the same way: restart without modules, the head, then the Apply of the modules
of the same build. The WebUI does it from the `.ota`; only how the head reaches the flash differs per chip
(`HEAD_ROUTES` of `install.mjs`):

| Chip | Restart | The head goes to |
|------|---------|------------------|
| ESP8266 | `POST /modules/off` | `/upgrade`; written behind the head, into the module area, eboot copies it at the restart |
| Beken | `POST /modules/off` | `/upgrade`, the head of the board's bootloader; written into the bootloader's `download` partition, which is the module area |
| ESP32 | `POST /safeboot/switch`, the board restarts into its safeboot | the safeboot's `/upload`; it writes app0 and boots ESPurna |

How each chip's update works below that (the safeboot, the bootloaders, the migration image for older ESP8266 images):
[docs/updates.md](updates.md).

## When a module goes wrong

The board protects itself and turns the modules off (the off mark in the directory, with the reason). While module code
runs, a watch on a 1-second timer (it keeps running when the main loop hangs) restarts the board at once when:

- free heap stays below 2 KB for 5 seconds;
- memory allocations keep failing for 20 seconds;
- the main loop has not come round for 20 seconds.

Also after three boots in a row crashed within their first 5 minutes with module code running. The count lives in the
directory sector (a pair of words per boot, written without an erase), the same on every chip: the Beken bootloader
clears the whole RAM at every reset, so nothing else survives a crash there.

### Steps of a board in trouble

The board goes down step by step and keeps the network as long as it can:

1. **Normal**: network and modules.
2. **Without modules**: network, WebUI (the head's page), telnet and OTA; the modules off with the reason shown. The
   board gets here
   - after crashes with module code running (the guard above);
   - when a boot follows a crash and one more crash would make it unstable (`SYSTEM_CHECK_MAX`);
   - when the power was switched on three times in a row, each time for less than 10 seconds (the light switch of a
     board in a wall);
   - after the unstable mode (step 3) ends.
3. **Unstable**: ESPurna's own access point `ESPURNA-xxxxxx` (192.168.4.1) and telnet, no network, never any module.
   After 10 minutes without a crash (`SYSTEM_CHECK_EXIT`) it restarts into step 2, not into the modules.

Ways back, from the cheapest:

- **Wait**: from the unstable mode the board comes back by itself after 10 minutes, with the network and without the
  modules; *Start the modules again* or an Apply brings them back.
- **The light switch** (a board in a wall): on and off three times, each time less than 10 seconds, then on.
- **Its access point**: join `ESPURNA-xxxxxx`, telnet 192.168.4.1: `MODULES.OFF`, `reset`, or an OTA from there.
- **ESP32 only**: the safeboot (its own image in the factory partition) takes a new ESPurna over the network even when
  ESPurna does not start ([docs/updates.md](updates.md#esp32-the-safeboot)).
- **A wire**: only when nothing above works.

| What happened | What the board does | Way back |
|---------------|---------------------|----------|
| No internet | head and modules keep running | the `.ota` from a PC |
| A module crashes the board | modules off after 3 crashed boots, head only | Apply without that module, or *Start the modules again* |
| Head of another build (an update without the Apply) | its modules are not started, shown as stale | Apply from the `.ota` of the head's build |
| Apply broken off | modules off with the reason, the old directory kept | Apply again |
| The bases of a module moved since the Apply (a flash map, the heap block at another address) | rewritten for this boot from its relocation maps; Beken: not started, *Apply again* | Beken: Apply again |

## Commands and endpoints

| Terminal | |
|----------|-|
| `MODULES` | the module list: where each one is, its state, free flash, the heap block and the fixed pool |
| `MODULES.OFF` | modules off from the next boot (the off mark), restart; also from the unstable mode |
| `MODULES.ON` | clear the off mark, restart with the modules |
| `MODULES.ENABLE <name>` / `MODULES.DISABLE <name>` | an installed module (and what it needs) on / off at the next restart |
| `MODULES.REMOVE <name>` | take a module that is off out of the directory |
| `MODULES.TEST.LOWHEAP`, `MODULES.TEST.CRASH` | test the guard |

| HTTP (Digest auth as `/upgrade`) | |
|------|-|
| `GET /modules` | JSON: head id (`md5`), image, the modules with their states and what they take, the off reason, flash and RAM meters, heap |
| `POST /modules/off` | the off mark, restart (the first step of the Apply and of a head update) |
| `POST /modules/apply` | `names=a,b,c`, then the modules file (EMC2): the Apply |
| `POST /modules/on` | clear the off mark, restart |
| `POST /modules/enable` | `name=`, `on=1` / `on=0`: at the next restart |
| `POST /modules/remove` | `name=`: take out a module that is off |
| `POST /modules/restart` | restart |
| `GET /modules.html` | the head's page |

## Building

```
cd code
pio run -c platformio_esp8266.ini -e esp8266
pio run -d safeboot-app                        # ESP32: the safeboot first, then
pio run -c platformio_esp32.ini -e esp32
py -3 -m platformio run -c platformio_beken.ini -e beken
```

One env per chip, one build: the env's own image is the head; the module objects (`custom_modules` of the ini on top of
the list in `head.h`) are compiled in the same build with `custom_module_flags` (into `mod-src/`, the libraries only the
modules use, `custom_module_libs`, into `libmod/`), then every module is linked against this head
([`scripts/module_link.py`](../code/scripts/module_link.py) and the chip's `<chip>_module_link.py`). When a module needs
a head function the head dropped, the link adds it to `custom_head_exports`, links the head again and the modules after
it, in the same build. Then [`module_initial_image.py`](../code/scripts/module_initial_image.py) writes the initial
image(s), [`module_ota.py`](../code/scripts/module_ota.py) the install image,
[`release_image.py`](../code/scripts/release_image.py) names the release files. Output in
`.pio/build_<chip>/<chip>/`: the release files, `module-<name>.module` / `.elf` / `.map`, `espurna-<chip>-modules.bin`.

## How it works

### The module area and its directory

Each chip has one module area in flash (where it lies: [per chip](#per-chip-what-the-hardware-forces)). Its last sector
is the directory (`EDR6`: up to 16 entries, each with its slot, size, CRC, RAM offset, the three bases its words hold
now, its flags; the off reason; a generation and a CRC), the sector under it a second copy: a write goes into the older
copy, a read takes the newest whole one, so a power cut loses only that write. The second half of the directory sector
holds the guard's boot marks (at 2048) and the power marks of the light switch (at 3072), written without an erase.
Modules lie from the top of the area down, in whole sectors, each followed in its slot by its relocation maps.

### The package

One format on every chip (version 4, `modules_core.h` `PackageVersion`): its header, then per 4 KiB chunk of the image
a map of 2 bits per word (0 as is, 1 flash data view, 2 RAM, 3 code), a branch list (Beken's `BL` / `BLX` calls into the
head, which are PC-relative and have to move with the module; empty on the other chips), then the chunk. The board
relocates each chunk as it arrives and writes it; the CRC is checked over what was received and what was written.

### The build

- **Head** first, the generic image; its identity is the MD5 of the image (ESP8266, ESP32) or the first 16 bytes of its
  GNU build id (Beken).
- **Each module** is linked separately against the head (`--just-symbols`, `--emit-relocs`, no relaxation) twice, at two
  sets of placeholder bases; the words that differ by the delta of one base are its relocations. Every call into the
  head goes through an absolute address, except Beken's Thumb `BL`, which goes into the branch list.
- **No second copy of stateful code.** A library member that the head links and that keeps state (interrupt table,
  timers, UART, the `MDNS` object) exists only in the head; other members may come with the module.
- **Modules call each other through the head.** Functions one module provides to others are forwarders in the head to
  slots the provider fills at its start; the provider's own symbols are renamed, so a call always lands in the head.
  Providers start first.
- Windows: the exports go to the linker as one script with `EXTERN()`, a long `-Wl,-u` list broke the command line.

### Boot

Before ESPurna's own setup: the guard's marks, the power marks, the step before the unstable mode; nothing starts in
the unstable mode or with an off mark. The heap block of the modules' statics is taken early, before the heap is cut
up, as large as the directory says. Then per entry: the head id, the CRC; when its bases moved since the Apply, the module is rewritten in place from
its maps (not on the Beken); `.data` copied, `.bss` zeroed, IRAM code copied (ESP8266), its init array run, its entry
called. Data modules (the WebUI) are only registered.

## Per chip: what the hardware forces

### ESP8266

    0x000000  head (eboot + the image)                       grows up
    ...       module area: modules from the top down
    0x0F8000  directory, second copy
    0x0F9000  directory
    0x0FA000  settings (EEPROM_Rotate pool), RF cal, SDK Wi-Fi

- **Area:** from the end of the head to the directory, the sector under the settings' rotation pool (computed at runtime
  from EEPROM_Rotate). A module runs from the flash mapped at `0x40200000`, 32-bit reads only.
- **Fixed pool:** the IRAM after the head's code (the core's 48 KB IRAM layout, 16 KB flash cache): the Apply puts the
  modules' IRAM code at its start, then statics. Only 32-bit access is native there; the core's `NON32XFER_HANDLER`
  emulates the rest, slower and never from an interrupt, so a module whose statics take byte access from an interrupt
  goes to the heap block. The head keeps 4 KB of heap for itself when the Apply sizes the block.
- **Heap reserve** (`arch/esp8266/heap_reserve_esp8266.cpp`): the SDK's allocators and `operator new` are wrapped; a failing
  allocation gets a 1.5 KB reserve and is tried again (without it the SDK wrote through a NULL pointer or `new` aborted
  the board). New connections are refused while less than 5 KB is free or three are open to the same port.
- **One layout for every chip size:** the 1 MB layout (`eagle.flash.1m.ld`), settings in sectors 251 / 250
  (`0xFB000` / `0xFA000`). Boards installed with the old 4 MB layout (`esp8266-4m` image, the HW-622 board build,
  1.15 builds for 4 MB) keep their settings at the end of the 4 MB (sectors 1012-1019) and move them once: settings
  backup (WebUI, *ADMIN*, *Backup*), the update, restore. On those boards `0xF9000`-`0xFFFFF` was part of the old sketch
  area and may hold bytes of an older image: settings with a bad checksum start as defaults, but the SDK's RF
  calibration and system parameter sectors (`0xFC000`-`0xFFFFF`) are not checked.

### ESP32

- **Area:** the tail of app0 behind the head (or a data partition named `modules`), the directory in its last sector.
  The safeboot layout leaves app0 2880 KB ([docs/updates.md](updates.md#esp32-the-safeboot)).
- **Maps:** the whole area is mapped twice at boot, before anything else of ESPurna maps flash: into the instruction bus
  (code, kind 3) and the data bus (header, `.rodata`, the `.data` image, kind 1). When the maps come out at other
  addresses at a boot, the modules are rewritten for them (the boot relocation above).
- **No fixed pool:** the statics go into the heap block; the head keeps 32 KB of heap for itself (Wi-Fi, web, TLS).
- Modules see only the symbols the head defines; no module uses TLS (the Matter module carries its own `ErrorStr`).

### Beken (BK7231N)

- **Area:** the bootloader's `download` partition (Tuya 2 MiB: `0x12A000`, 664 KiB; Quectel 4 MiB: `0x286000`). Its
  first sector stays free (the RBL header the bootloader looks at), its last sector is the directory. The head's own
  OTA image goes there too, so a head update needs the modules off.
- **Flash format.** The CPU reads flash through the controller's mapping (XIP): logical address `L` is stored at flash
  offset `L / 32 * 34`, every 32 bytes followed by their CRC-16, the words encrypted with the chip's key. The board
  writes modules in that format itself. The key is found at boot by comparing a block of the running app read raw with
  the same block read through the mapping (the Tuya key, or the zero key of unencrypted boards); an unknown key means no
  modules. A module starts at the first 32-byte boundary of its sectors.
- **No rewrite at boot.** The 34-byte blocks cross the sectors, a rewrite in place needs the whole image in RAM: when a
  module's bases moved since the Apply it stays off with *Apply again*.
- **Read cache.** The mapping caches 16-32 KiB and nothing in the SDK flushes it: after writing a module the loader
  reads 64 KiB of the app before it checks the module through the mapping. A block with a bad CRC read through the
  mapping stalls the chip, so every check reads the blocks raw first.
- **Fixed pool:** the data TCM the head leaves free (60 KiB TCM next to the 192 KiB main RAM; the Wi-Fi driver and the
  head's `.bss` moved there by `scripts/beken_tcm.py` take the rest). The head keeps 8 KB of heap for itself.
- **Matter as a module** needs from the head lwIP with IPv6 and the Matter library's lwIP options
  ([beken/README.md](../beken/README.md#2-matter)).
