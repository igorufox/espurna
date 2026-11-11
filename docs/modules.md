# Modules: a small head image and the parts you switch on

**Status.** ESP8266 1 MB: prototype, works on two bench Sonoff Basic boards. ESP32 and Beken: in progress, see
[ESP32](#esp32) and [Beken](#beken). One build per chip makes the head, every module and the release files; CI
(`.github/workflows/esp32.yml`) builds the three and publishes them (see [Building](#building)). How it works inside: [How it works](#how-it-works).

The rest of this page is the ESP8266; the ESP32 and Beken work the same way for the user (the same MODULES page, the
same commands), the differences are in their sections.

## What it is

A board runs the **head**, a generic 1 MB image with what every board needs: Wi-Fi, relays, buttons, LEDs, the web
server, MQTT, API, telnet, NTP, OTA. Everything else is a **module**: a separate piece of code, or the WebUI page, that
the board writes into the free flash behind the head and starts at boot. You switch modules on and off on the board's
MODULES page; nothing has to be built for a board.

What it gives a 1 MB board:

- **updates without the mini.** The head is small enough for its `.bin.gz` to fit behind it, so one upload updates it
  (the modules are removed first and put back after);
- **one image** instead of `esp8266-1m`, `-1m-light` and `-1m-sensor`, and boards that needed two of them (light +
  power meter, light + RF) get both;
- **a board that always answers.** Without modules, with a module of another build, or after a module crashed it, the
  board runs the head: relays, buttons, MQTT, API, telnet and its own small page keep working.

## Where it is going

The owner's rules for the design (2026-11-09); what is not done yet is marked so.

- **One mechanism on every chip.** ESP8266, ESP32 and Beken share the loader core; a chip only adds its backend.
- **Three builds, one per chip.** Each makes the head, every module and the release files; no feature variants.
- **Two images per chip:** *initial* (the head) and *full* (the head with every module already in the flash).
- **Everything off until someone needs it.** In *full* the modules lie in the flash switched off and take no RAM;
  switching one on gives it its RAM then, without a download and without a restart. So the full image cannot fail on
  RAM at build time. (ESP8266: built, the full image has its 11 modules off with no RAM taken and 40 KB of flash
  free; switching on from it is not tested on a board yet. ESP32 and Beken: next.)
- **An empty head.** Everything goes to modules; the head keeps relays, buttons, LEDs, Wi-Fi, web, MQTT, OTA and
  what reaches a broken board: telnet, the API, NTP.
- **Dependencies.** A module that needs another (Domoticz needs MQTT) switches it on with itself; a module cannot be
  switched off or removed while one that is on needs it.
- **Smaller modules.** `sensors` and `light` are to be split into many small ones; the Tuya dimmer becomes a module;
  with many modules the panel groups them.
- **4 MB boards** move to the new layout (the 1 MB head, settings carried over).

## Modules

| Module | What it brings | Flash | RAM | IRAM code |
|--------|----------------|------:|----:|----------:|
| `webui` | the full WebUI (without it the head shows its own small page) | ~68 KB | | |
| `sensors` | the sensor manager and the drivers of `esp8266-1m-sensor` (HLW8012, CSE7766, PZEM, DHT, Dallas, BMx280, analog, NTC, digital ...) | ~92 KB | 2.9 KB | 1.1 KB |
| `light` | lights: PWM, my92xx | ~40 KB | 1.5 KB | |
| `rf` | 433 MHz RF bridge (rc-switch) | ~16 KB | 1.3 KB | 0.8 KB |
| `scheduler` | the scheduler | ~29 KB | 0.5 KB | |
| `alexa` | Alexa (local, FauxmoESP) | ~22 KB | 0.6 KB | |
| `mdns` | mDNS (`<hostname>.local`) | ~25 KB | 0.2 KB | |
| `homeassistant` | Home Assistant MQTT discovery | ~15 KB | 0.3 KB | |
| `domoticz` | Domoticz | ~8 KB | 0.2 KB | |
| `thingspeak` | Thingspeak | ~7 KB | 0.2 KB | |

Sizes of the 2026-11-08 build; every build differs a little.

Modules that use each other do it through the head: Home Assistant, Domoticz and Thingspeak show the sensors' values
when `sensors` is on, the scheduler and Alexa drive the lights when `light` is on. Without the other module they simply
have less to show.

The limit is RAM, not flash. Every module's variables come from two fixed areas of the head: the **RAM arena** (5 KB)
and the **RAM in IRAM** (17.5 KB of the instruction RAM the head leaves free). The heap pays too: a module's start
takes some (`sensors` ~0.5 KB, `alexa` ~0.8 KB, the others less), and every request to the web server costs several
KB while it runs. The head alone has 15-17 KB of free heap at rest; with `sensors` and four more modules 6-9 KB: the
WebUI works but slowly, one browser tab at a time. All ten modules fit the flash, not the heap. Switch on what the
board needs.

## Files

| File | What | For |
|------|------|-----|
| `espurna-esp8266.bin` / `.bin.gz` | the head ("initial"), the 1 MB layout on every chip size | serial at `0x0`, or the `.bin.gz` over the web from ESPurna (through the mini from a full image of an older release on a 1 MB board) |
| `espurna-esp8266-full.bin` | the head with all modules already in the flash, and the modules file behind 1 MB (all of them off, no RAM taken until switched on) | serial at `0x0` on a 4 MB board, or anything that should have everything at once; also a source of modules for the MODULES page |
| `espurna-esp8266-modules.bin` | the modules file: every module of the build | the source the MODULES page takes modules from |

Modules belong to the head they were built with: a module of another build is not started (the page shows it as
*stale*). After a head update, take the modules from the same build.

## Switching modules on and off

**MODULES** in the WebUI menu (between ADMIN and DEBUG). Without the `webui` module the head answers on `/` with
its own page (`/modules.html`), which does the same.

1. **Source.** Pick `espurna-esp8266-modules.bin` or `espurna-esp8266-full.bin` from the disk. A 4 MB board
   flashed with `full` has the modules file in its own flash: the page takes it by itself. *From GitHub* takes the
   file of the running build from the release list once CI publishes it.
2. **Switch** a module on or off. The page shows what it takes of the flash, the RAM and the IRAM, and the meters
   what is left: yellow from 85 %, red when it does not fit. Pointing at a module shows its share on the meters before
   you switch it on.
3. **RAM: DRAM or IRAM.** Each module's variables go to the RAM arena (DRAM) or to the RAM in IRAM. IRAM has much more
   room but takes only 32-bit access; byte access is handled by the core, slower, and never from an interrupt: `light`
   and `rf` (interrupt code) stay in DRAM. `sensors` in IRAM is the usual choice. The choice can be changed on a
   module that is already on: the board writes a second copy next to the old one and switches over at the restart.
4. **On** starts the module at once (a module that is not on the board yet is written into the free flash first),
   together with the modules it needs. **Off** keeps it in flash and stops it at the next restart (the yellow bar
   *Restart now*); it is refused while a module that is on needs it. **Remove** takes a module that is off out of the
   flash. Changing the RAM of a running module also waits for the restart.

The board does the placing itself: the page (or any other client) only uploads the module, the board picks the free
flash and RAM for it. No step needs GitHub or internet: the files can be on a PC.

## Updating the head

While modules run, an update is refused (`409 modules installed, MODULES.OFF first`): it would overwrite the code the
board is running.

1. `MODULES.OFF` in the terminal (DEBUG page, telnet or API): the board removes its modules and restarts on the head.
2. Upload the new `espurna-esp8266-1m-initial.bin.gz` (ADMIN, upgrade): it fits behind the head, no mini step.
   With little free heap the upload can answer `Out of memory` once (it needs a 4 KB buffer in one piece): upload
   again.
3. On the MODULES page, switch the modules on again from the modules file of the same build (they start at once).

Relays, MQTT and the API work through the whole update; the WebUI is back with step 3 (until then the head's page).

## When a module goes wrong

The board protects itself and turns the modules off. A watch on a 1-second SDK timer (it keeps running when the main
loop hangs) restarts the board at once when, with module code running:

- free heap stays below 2 KB for 5 seconds;
- memory allocations keep failing for 20 seconds;
- the main loop has not come round for 20 seconds.

Also after three boots in a row crashed within the first minute with the modules running.

Before it comes to that, the head keeps a 1.5 KB heap reserve for the Wi-Fi SDK and for `new`: an allocation that
fails gets the reserve and is tried again (without it the SDK wrote through a NULL pointer, or `new` aborted the
board with *OOM*). New connections are refused while less than 5 KB is free or three are already open to the same
port.

The board then runs the head only. The MODULES page (or the head's page) says why, in red, with *Start the modules
again*; the terminal command is `MODULES.ON`. ESPurna's unstable mode (after crashes) never starts modules either.

| What happened | What the board does | Way back |
|---------------|---------------------|----------|
| No internet | head and modules keep running | modules from a file on a PC |
| Module upload broken off, power cut | the module is not listed, or its check sum fails: not started | upload it again |
| A module crashes the board | modules off after 3 crashed boots, head only | fix or leave that module off, *Start the modules again* |
| Head updated, modules of the old build | not started, shown as stale | switch them on from the new build's file |
| Head update stopped half-way | new head, no modules, the head's page on `/` | switch the modules on from the file |
| Apply broken off (power cut, the page closed) | modules off with the reason (the old directory stays: the new one is written only at the very end), but the new packages may already lie over the old ones: those fail their check sum and are not started | Apply again |

### Steps of a board in trouble, and the ways back without a soldering iron

The board goes down step by step and keeps the network as long as it can:

1. **Normal**: network and modules.
2. **Without modules**: network, WebUI, telnet and OTA, the modules off with the reason shown (the off mark of the
   directory). The board gets here
   - after crashes with the modules running (the guard above);
   - when a boot follows a crash and one more crash would make it unstable (`SYSTEM_CHECK_MAX`);
   - when the power was switched on three times in a row, each time for less than 10 seconds (the light switch of a
     board in a wall);
   - after the unstable mode (step 3) ends.
3. **Unstable**: its own access point `ESPURNA-xxxxxx` (192.168.4.1) and telnet, no network, never any module. After
   10 minutes without a crash it restarts into step 2, not into the modules.

Ways back, from the cheapest:

- **Wait**: from the unstable mode the board comes back by itself after 10 minutes, with the network and without the
  modules; the MODULES panel says why, *Start the modules again* or an Apply brings them back.
- **The light switch** (a board in a wall): on and off three times, each time less than 10 seconds, then on: the board
  comes up without the modules.
- **Its access point**: join `ESPURNA-xxxxxx`, telnet 192.168.4.1: `MODULES.OFF`, `reset`, or an OTA from there.
- **ESP32**: the safeboot (its own small image in the factory partition) takes a new ESPurna over the network even
  when ESPurna does not start; it opens its own access point when it cannot join the Wi-Fi.
- **A wire**: only when nothing above works (no access point, no network at all).

## Commands and endpoints

| Terminal | |
|----------|-|
| `MODULES` | the module list: where each one is, its state, free flash, the arenas |
| `MODULES.OFF` | remove all modules, restart (before a head update) |
| `MODULES.ENABLE <name>` / `MODULES.DISABLE <name>` | switch a module (and what it needs) on at once / off at the next restart, it stays in flash |
| `MODULES.REMOVE <name>` | take a module out of the flash |
| `MODULES.ON` | start the modules again after the guard turned them off |

| HTTP (Digest auth as `/upgrade`) | |
|------|-|
| `GET /modules` | JSON: modules, states, flash and RAM use, the guard's reason; heap: `heap` (measured inside the request, lower by its own cost), `heap_loop` (as the main loop sees it), `heap_low` (lowest of the last 1-2 minutes), per module the heap its start took; the reserve: `reserve`, `reserve_used`, `oom`, `refused` |
| `POST /modules/upload` | a module package or the modules file (`?names=a,b` picks parts), `?ram=iram`, `?replace=1` (new copy of an installed module) |
| `POST /modules/enable` | `name=`, `on=1` (start now, with what it needs) or `on=0` (off at the restart, stays in flash) |
| `POST /modules/remove` | take a module out of the flash |
| `POST /modules/on` | start the modules again |
| `POST /modules/restart` | restart |
| `GET /modules/file` | the modules file in the board's own flash (4 MB boards with `full`), 404 otherwise |
| `GET /modules.html` | the head's page |

## Building

```
cd code
pio run -c platformio_esp8266.ini -e esp8266
pio run -c platformio_esp32.ini -e esp32           # the safeboot first: pio run -d safeboot-app
pio run -c platformio_beken.ini -e beken
```

One env per chip, one build: the env's own image is the head; the objects of `custom_modules` are compiled in the same
build with `custom_module_flags` (into `mod-src/` next to the head's `src/`, the libraries only the modules use,
`custom_module_libs`, into `libmod/`), then every module is linked against this head
([`scripts/module_link.py`](../code/scripts/module_link.py) and the chip's `<chip>_module_link.py`). When a module needs
a head function the head dropped, the link adds it to `custom_head_exports`, links the head again and the modules
after it, in the same build. Output in `.pio/build_<chip>/<chip>/`: the release files, `module-<name>.module` /
`.elf` / `.map`.

### The ESP8266 flash layout

One head image for every ESP8266: the 1 MB layout (`eagle.flash.1m.ld`) on every chip size, the settings in sectors
251 / 250 (`0xFB000` / `0xFA000`), the module directory at `0xF9000`; behind 1 MB on bigger chips the modules file
(`full`). Boards installed with the old 4 MB layout (`esp8266-4m` image, the HW-622 board build, 1.15 builds made for
4 MB) keep their settings at the end of the 4 MB (sectors 1012-1019) and move them once: settings backup (WebUI,
*ADMIN*, *Backup*), the update, restore. On those boards `0xF9000`-`0xFFFFF` was part of the old sketch area and may
hold bytes of an older image: settings with a bad checksum start as defaults, but the SDK's RF calibration and system
parameter sectors (`0xFC000`-`0xFFFFF`) are not checked. A serial flash of `espurna-esp8266-full.bin` sets the whole
first 1 MB; the move over the web is not tested on a board yet.

## ESP32

**Status (2026-11-09): the head and all modules run on the lab board (LC-Tech ESP32 relay x2), installed by a
script; the MODULES panel on the ESP32 is not tested yet.**

What it is for: one ESP32 image instead of `esp32-matter` and `esp32-matter-nspanel`; Matter, the NSPanel and the
integrations only on the boards that use them.

- **The head** (`esp32`): relays, buttons, LEDs, Wi-Fi, the web server with the embedded WebUI (with the MODULES
  panel, so no `webui` module on the ESP32), MQTT, API, telnet, mDNS, the safeboot, the module loader. 1.5 MB
  (the image with Matter inside was 2.3 MB). Free heap without modules: 139 KB (38 KB with Matter in the image).
- **Modules:**

  | Module | Flash | RAM |
  |--------|------:|----:|
  | `matter` | 950 KB | 61.6 KB (the Matter stack's static pools) |
  | `nspanel` (display protocol, pages, TFT upload) | 367 KB | 5.3 KB |
  | `scheduler` | 51 KB | 0.2 KB |
  | `sensors` | 38 KB | 0.4 KB |
  | `alexa` | 25 KB | 0.2 KB |
  | `homeassistant` | 17 KB | 0.2 KB |
  | `thingspeak` | 9 KB | 0.3 KB |
  | `domoticz` | 8 KB | 0.1 KB |
  | `test` | 1 KB | 0.1 KB |

- **Matter or the NSPanel, not both:** together they need more than the 64 KB RAM limit, and the heap would run out.
  Tested: the six integrations + `nspanel`, 117 KB free; the six integrations + `matter`, Matter started with its
  controllers kept, 31-37 KB free (lowest 12.8 KB).
- **Where they live:** the free tail of the app partition behind the head (1.3 MB with this head), its last sector is
  the module list. No partition change, so no safeboot migration.
- **RAM:** one area taken from the heap at boot, as large as the installed modules need (64 KB at most), only when a
  code module is installed. No IRAM choice.
- **NSPanel pins** are settings now: `nspTx`, `nspRx`, `nspPower`, `nspBuzzer` (-1 = none; a pin a relay uses is not
  taken); the defaults are the NSPanel's.
- **Switching on:** the same endpoints as on the ESP8266 (an install takes 0.4-30 s, `matter` the longest), from the
  modules file `espurna-esp32-modules.bin` (all 9 modules, 1.56 MB) picked from the disk. There is no modules
  file in the board's flash (`/modules/file` answers 404).
- **Head update:** through the safeboot, as every ESP32 update; it rewrites the app partition, so the modules are gone
  or left stale afterwards: switch them on again from the same build's modules file.
- **Build:** `pio run -c platformio_esp32.ini -e esp32` (the head `espurna-esp32.bin`, every module and the modules
  file); output in `.pio/build_esp32/esp32/`.

Tested on the lab board: install over `/modules/upload`, restart, every module *running*; the RAM area grows with the
installed set and lands at another address, all modules were relocated for it and started; a module installed before
there was an area moves into it at the next boot; a module of another head is *stale* and does not start; a removed
module keeps its flash until the restart. The NSPanel module ran its display protocol and mounted its file system on
a board without a display (relays untouched); a real panel is not tested.

On the shared loader core (head 1.5 MB, 137.6 KB free heap, 1.3 MB for modules): `test`, `scheduler`, `alexa`,
`homeassistant`, `domoticz`, `thingspeak` installed in 0.3-3 s each and started at once, all running after the
restart with their RAM moved into the area, 130 KB free heap with the six; the WebUI MODULES panel reads the board
without errors. Matter switches IPv6 on through the shared Wi-Fi code (`wifiIpv6()`). Not tested yet on the core:
`sensors`, `matter`, `nspanel`, an install from the panel's switch, the guard on the ESP32.

## Beken

**Status (2026-11-09): the head and three modules, Matter among them, run on a bench board** (BK7231N, 2 MiB, Tuya bootloader, the
[square relay](../beken/86-tdq.md#the-same-relay-with-the-chip-on-the-board-bare-bk7231n-qfn32)).

What it is for: Matter as a module. The Matter code (~490 KB) does not fit next to ESPurna in the app partition of a
2 MiB board; as a module it goes into the space behind the head.

- **The head** (`beken`): all integrations except Thingspeak and Domoticz, RF, the embedded WebUI (with the
  MODULES panel). Its update image is 624 KiB of the 664 KiB update partition: 40 KiB left to grow, after that the
  WebUI or more integrations have to leave the head.
- **Modules:** `thingspeak` (5.4 KB), `domoticz` (7.7 KB) and `matter`: the Matter stack and ESPurna's Matter code,
  480 KB, RAM 36.8 KB, installs in about 14 s. The head carries what Matter needs from it: lwIP with IPv6 and the
  Matter library's lwIP settings, no mDNS of its own (Matter's stack answers mDNS), a 38 KiB RAM area in the on-chip
  RAM (the 4 KiB settings buffer moved to the main RAM for it); that head is 958 KB. All three modules: 136 KiB of
  flash and 1.9 KiB of the RAM area left.
- **Matter from the module** works as in the one-piece image: `MATTER.*` commands, `matterAuto 1` starts it after
  the boot, its own identity (development attestation, serial = chip id) on boards without factory data. Free heap
  27.7 KB with Matter running (lowest 17.7 KB), 38.9-40 KB after boot without it.
- **Where they live:** the bootloader's update partition (656 KiB for modules), its last sector is the module list.
- **RAM:** a fixed area of the head in the fast on-chip RAM (16 KiB, 38 KiB in the head for Matter); it costs the
  heap nothing.
- **Switching on:** the same endpoints and commands as on the ESP8266, from the modules file
  `espurna-beken-modules.bin`; an install takes under a second. The WebUI panel on the Beken is not tested yet.
- **Head update:** `/upgrade` answers 409 while modules run; `MODULES.OFF`, upload the new head's `.uf2`, switch the
  modules of the new build on again. The head update goes into the space the modules had.
- **Build:** `pio run -c platformio_beken.ini -e beken` (the head `espurna-beken-tuya.uf2`, every module and the modules
  file; output in `.pio/build_beken/beken/`).

Tested on the bench board: install over `/modules/upload`, both modules running after the restart (their WebUI keys,
a relay change reaching Thingspeak), a second copy of a running module placed and started, a damaged package and a
module of another head refused, the head update with `MODULES.OFF` and the modules of the new build installed again;
the guard (twice, also on the shared loader core): three boots in a row crashed within 60 s with the modules running
(the unstable mode in between included) turned them off with the reason in the module list, `MODULES.ON` brought them
back. Matter from the module: commissioned with a matter-server controller (basic information, IPv4 and two IPv6
addresses), On/Off both ways (the controller switches the relay, the relay reports back), started again by itself after
a restart with `matterAuto 1`, the controller kept. Not tested: other Matter controllers, boards with the Quectel
bootloader (4 MiB, no encryption), the MOES bootloader 3.00 (no update partition for us), a power cut during an
install, the WebUI panel (the modules were installed by a script).

## How it works

One loader core for every chip (`espurna/modules.cpp`: the module list, upload and placing, `/modules`, the guard,
the `MODULES*` commands) with a backend per chip (`modules_esp8266.cpp`, `arch/esp32/modules_esp32.cpp`,
`arch/beken/modules_beken.cpp`: flash access, mapping, RAM areas), and one link script (`scripts/module_link.py`, a
`Linker` class each chip extends). The parts below are per chip.

### ESP8266 flash map (1 MB)

    0x000000  head (eboot + the image)                       grows up
    ...       free
    ...       modules, 4 KB aligned, best fit                grows down
    0x0F8000  module list, second copy
    0x0F9000  module list
    0x0FA000  settings (EEPROM_Rotate pool), RF cal, SDK Wi-Fi

The module list is the sector under the settings' rotation pool, computed at runtime (`base() - size()` of
EEPROM_Rotate): inside the pool a settings write erased it. It also holds the guard's reason, so the decision
survives crashes and an older settings copy. Since 2026-11-09 evening it is written twice with a checksum, the second
copy in the sector under it (not tested on a board yet).

### Build: the head links alone, each module links against it

- **Head** first, the generic image without the module code; its identity is the MD5 of the image.
- **Each module** is linked separately against the head (`--just-symbols=head.elf`, `--emit-relocs`, `--no-relax`) at
  placeholder bases for its flash, RAM and IRAM. With `-mlongcalls` and no relaxation every call into the head goes
  through an absolute literal, never a PC-relative `CALL0` that would break when the module moves. The package holds
  the bytes and the word offsets that carry an address of the module itself (per kind: flash, RAM, IRAM).
- **Head exports.** `--gc-sections` drops head functions only a module uses; the module link writes them to
  `modules_exports.txt` and the head keeps them (one more build of the head, the link says so).
- **No second copy of stateful code.** A library member that the head links and that keeps state (interrupt table,
  timer1, UART, the `MDNS` object) exists only in the head; other members may come with the module.
- **Modules call each other through the head.** Functions one module provides to others (magnitudes of `sensors`,
  33 light functions, `domoticzSendMagnitude` ...) are forwarders in the head to slots the provider fills at its start;
  the provider's own symbols are renamed, so a call always lands in the head. Providers start first (priority).
- Windows: the exports go to the linker as one script with `EXTERN()`, a long `-Wl,-u` list broke the command line.

### Install and boot

- The board takes the package as it arrives, picks the slot (best fit in the flash, in the RAM arena, in the IRAM
  arena or the RAM in IRAM), adds the slot bases to the listed words on the way and writes sector by sector; CRC32
  over what was received and what was written; the list entry is written last (the commit).
- **Off without RAM** (flag `EntryUnplaced` in `modules_core.h`; ESP8266 built, not tested on a board yet): a module that
  is off and not running gives its RAM back (`arch::placeable`); switching it on makes the core call `arch::place`,
  which takes RAM for it and lays its image out again in its own slot from the package kept on the board. A module
  switched off while running keeps its RAM until the restart. Backends without it (ESP32, Beken for now) keep the RAM
  of every installed module.
- At boot, before ESPurna's own setup: skip everything in unstable mode or with a guard reason; per entry the head's
  MD5, the CRC, copy `.data` into the arena, zero `.bss`, IRAM code into its arena, run the module's own init array,
  call its entry.
- **Heap reserve** (`heap_reserve_esp8266.cpp`, head only): the SDK's allocators (`pvPortZalloc`,
  `sdk3_pvPortMalloc`, `pvPortRealloc`) and `operator new` / `new[]` are wrapped with `--wrap`
  (`scripts/esp8266_head_exports.py`); a `tcp_accept` gate drops new connections when the heap is short or a port
  has three. The guard checks run on an SDK timer, not in `loop()`, and restart with `system_restart()` right away.
- **RAM in IRAM**: the head runs the core's 48 KB IRAM layout with `NON32XFER_HANDLER`; the IRAM after its code takes
  module variables. Only 32-bit access is native, the rest goes through the core's exception handler.

### ESP32

The modules sit in the tail of app0 and are mapped twice at boot, before anything else maps flash: once into the
instruction bus, once into the data bus (`esp_partition_mmap` INST and DATA). A module is linked with three bases:
data view (header, `.rodata`, the init image of `.data`), RAM (`.data` + `.bss`), instruction view (code). The maps
land at the same addresses for a given head; each list entry keeps the bases its words hold, and when a map or the RAM
area moved, the loader relocates the module again from the maps kept behind it in flash. The RAM area is taken from
the heap at boot, sized for the installed modules (64 KB at most), only when a code module is installed. Package
version 2 (the ESP8266 loader refuses it). Modules see only the symbols the head defines; the Matter module carries
its own copy of `ErrorStr` (without `thread_local`); no module uses TLS.

### Beken

- **Flash format.** The CPU reads flash through the controller's mapping: logical address `L` is stored at flash
  offset `L / 32 * 34`, every 32 bytes followed by their CRC-16, the words XORed with a pad made from `L` and the chip's
  key. The board writes modules in that format itself. The key is found at boot by comparing a block of the running
  app read raw with the same block read through the mapping (the Tuya key or the zero key of unencrypted boards); an
  unknown key means no modules. A module takes whole sectors and starts at the first 32-byte boundary of its sector.
- **Read cache.** The mapping caches 16-32 KiB and nothing in the SDK flushes it: after writing a module the loader
  reads 64 KiB of the app before it checks the module through the mapping. A block with a bad CRC read through the
  mapping stalls the chip, so every check reads the blocks raw first.
- **Update partition.** Its first sector stays free (the bootloader looks at it), its last sector is the module list
  (also the guard's boot marks), modules from the top down in whole sectors.
- **Relocation for Thumb.** The module is linked twice at two flash and two RAM bases; the package (version 3) holds the
  words that hold an address of the module and a list of the `BL` / `BLX` calls into the head, which are PC-relative and
  have to move when the module moves (taken from the relocation records and from the linker's interworking veneers).
  The RAM area is fixed for a head build, so a module is relocated once, at the install.
- **Head identity:** the first 16 bytes of the head's GNU build id.
- **Guard.** The bootloader clears the RAM at every reset, so the crash count lives in the module list sector: a pair
  of words per boot with module code (*started*, then *finished* after 60 s or a normal restart), cleared bit by bit
  without an erase. Three unfinished boots in a row turn the modules off. The restart that ends the unstable mode does
  not count as a clean end.
- An upload takes about 8.6 KB of heap.

## Long run (ESP8266)

Sonoff Basic with the head and `scheduler`, `alexa`, `homeassistant`, `webui`, 2 h 5 min under load: `/modules` every
minute, the WebSocket terminal (uptime, heap, a relay switch) every 5 minutes, the head's page every 15 minutes, the
WebUI with the MODULES panel in a headless browser every 20 minutes, a module replaced (`replace=1`) every hour.

- **No leak.** 124 heap readings as the main loop sees it: median 10.5 KB in the first half hour, 11.1 KB in the
  last, the trend +29 bytes an hour (noise); lowest 6.3 KB, highest 16 KB.
- No restart (2 h 5 min of uptime in a row), no crash, the guard never fired.
- 18 failed allocations in 2 hours, the SDK reserve never needed, 2 connections refused.
- The WebUI with the panel loaded 5 times of 6 (one took longer than 60 s); the module replacement worked 2 of 2.

## Not tested yet (ESP8266)

- a 4 MB board with `full` (no 4 MB bench board): flashing at `0x0` and the modules file from its own flash;
- a power cut during a module upload;
- Home Assistant, Domoticz and Thingspeak against real servers;
- ArduinoOTA while modules run: it removes the module list and goes on (the module code runs until the restart).
