# ESP8266 1 MB: a head image and installable modules (plan)

Status: **prototype working on two bench boards** (2026-11-08), see *Prototype status* below.

## Idea

One build of every CI run makes a **head** (the image every 1 MB board runs) and a set of **modules** (sensor drivers,
light, IR, RF, relay MCUs, integrations). A board gets the head, then the WebUI adds the modules it needs, one after the
other, into the free flash behind the head, as long as there is room. A board without a module, or with a module of
another build, runs the head alone: relays, buttons, LEDs, Wi-Fi, web, MQTT, OTA.

What it replaces:

- the three 1 MB images `esp8266-1m`, `-1m-light`, `-1m-sensor` (one head + modules; also the boards that need two
  of them today, the Smartlife socket with light + power meter, the MagicHome with light + RF);
- the mini step of every update on 1 MB boards: the head is small enough for its `.bin.gz` to fit behind itself once
  the modules are gone (the mini stays for boards still on old firmware).

## Measured (generic 1 MB images of this tree, 2026-11-08)

Builds of `platformio.ini` envs with `-j 8` in an own build dir (`.pio/build-claude-modules`). Sizes from the linker
maps (`irom` = code and PSTR strings in flash, `iram`, `dram` = `.data` + `.rodata`, `bss`), per object or library;
the largest size of each over the three images.

| Image | `.bin` | `.bin.gz` (`gzip -9`) |
|-------|--------|------------------------|
| `esp8266-1m` | 756 192 | 544 582 |
| `esp8266-1m-light` | 812 304 | 571 993 |
| `esp8266-1m-sensor` | 774 048 | 542 704 |

Split by owner (objects and libraries assigned to a candidate module by name, everything else is the head):

| Group | irom | iram | dram | bss | What |
|-------|-----:|-----:|-----:|----:|------|
| head | 608 780 | 29 666 | 8 072 | 30 572 | SDK, lwIP, core, Wi-Fi, web server + **embedded WebUI**, WS, MQTT, relays, buttons, LEDs, settings, OTA, terminal, NTP |
| of it: the WebUI (`webui_data`) | 85 354 | | | | `all` preset in `esp8266-1m`; `sensor` 49 416, `small` ~46 K |
| ir | 69 960 | 287 | 450 | 225 | `ir.cpp` + IRremoteESP8266 (60 K) |
| sensors | 69 244 | 338 | 1 451 | 720 | `sensor.cpp` (56 K: drivers ~20 K, manager ~36 K), OneWire, HLW8012, I2C, Wire, `fs_math` |
| light | 25 852 | 369 | 1 103 | 581 | `light.cpp`, PWM, my92xx, encoder |
| mdns | 21 894 | 103 | 196 | 96 | ESP8266mDNS |
| scheduler | 21 866 | 0 | 187 | 24 | |
| api | 11 739 | 30 | 120 | 44 | |
| rf | 10 709 | 749 | 851 | 392 | `rfbridge.cpp` + rc-switch |
| homeassistant | 9 879 | 0 | 223 | 69 | |
| alexa | 9 248 | 0 | 286 | 173 | + FauxmoESP |
| relay-mcu | 8 211 | 0 | 362 | 230 | KingArt, iFan, LightFox (Dual / STM are inside `relay.cpp`) |
| domoticz | 6 609 | 0 | 72 | 9 | |
| telnet | 4 499 | 0 | 0 | 4 | |
| thingspeak | 4 191 | 0 | 4 | 191 | |

The embedded WebUI is already gzip data: it does not compress again. The code alone compresses to about 0.685
(`esp8266-1m`: 671 K of code -> 459 K).

Also measured on `-1m-sensor`: IRAM `.text` + `.text1` 30 571 of 32 768 bytes (about 2.2 KB free), heap region
(`0x3FFFC000 - _heap_start`) 40 008 bytes. Inside `sensor.cpp.o` (56 KB) the driver classes are about 20 KB; the rest
is the sensor manager (magnitudes, reporting, settings, WebUI, MQTT).

## Budget on 1 MB

The sketch area is `0x0`-`0xFB000` (1004 KB; settings sector, RF calibration and the SDK Wi-Fi data follow).

The head update without the mini: the board erases its modules, and the new head's `.bin.gz` has to fit behind the
running head, minus one sector (`ota_web.cpp`: free space - 4 KB):

    roundup(head) + gz(new head) + 4 KB <= 1004 KB,   gz ~ 0.685 x head (code only)   =>   head <= ~593 KB

Everything above the head is module space: 1004 KB - head - 4 KB (module directory).

What that means with the measured numbers:

| Head | Size | `.gz` | Head + `.gz` + 4 KB | |
|------|-----:|------:|--------------------:|-|
| the head group above, `all` WebUI embedded | ~646 K | ~470 K | ~1120 K | does not fit |
| with the `small` WebUI embedded | ~607 K | ~430 K | ~1041 K | does not fit |
| no WebUI in the head, sensor manager in the sensors module, API and telnet out | ~561 K | ~384 K | ~949 K | fits, 55 K to spare |
| **no WebUI, sensor manager in the sensors module, API and telnet in the head** | ~577 K | ~395 K | ~976 K | **fits, 28 K to spare** |
| no WebUI, sensor manager in the head | ~597 K | ~409 K | ~1010 K | does not fit |

**API and telnet stay in the head** (16 K): they are the way to a board when something went wrong (no WebUI module, no
GitHub, a script from the LAN), so they cannot be modules.

So:

- **The WebUI is a module too**, a data module (the gzip page of the release, no code, no relocation). The head keeps a
  small page of its own (status, upload, *install modules from GitHub*, like the mini's page, a few KB) for a board
  that has no WebUI module yet. With ~577 K of head there are ~423 K for modules: the `all` WebUI (85 K) plus sensors,
  light, IR, RF, relay MCUs and the integrations above without API and telnet (~258 K) is ~343 K, all of it at once.
- **The sensor manager goes into the sensors module** with the drivers. The integrations that read magnitudes (Home
  Assistant, Domoticz, Thingspeak, InfluxDB, Prometheus, RPN rules, thermostat) get them through a small magnitude
  interface of the head's registry, which the sensors module fills; without it they have no magnitudes.
- Every KB in the head counts twice (the head and its `.gz`). The estimate is from the object sizes; the real head
  link comes first in the prototype.

## Flash map

    0x000000  head (eboot + the ESPurna image as today)          grows up
    ...       free
    ...       module N   (4 KB aligned)                          grows down
    ...       module 1
    0x0FA000  module directory (one sector)
    0x0FB000  settings (EEPROM_Rotate), RF cal, SDK Wi-Fi         unchanged

Modules are placed from the top down, so the head can grow between releases without moving them. The directory lists
each module: name, flash offset, size, CRC32, its RAM / IRAM arena offsets, and the build id of the head it was made for.

## Build: the head links alone, the modules link against it

Not one ELF cut in pieces. Per CI run, one PlatformIO env:

1. **Head**: the generic image without the module code. Its build id (`-Wl,--build-id`) identifies the run.
2. **Each module**: its objects linked separately against the head (`-Wl,--just-symbols=head.elf`), with `-q`
   (`--emit-relocs`) and `--no-relax`, at a placeholder flash base, its RAM at a placeholder RAM base, its IRAM code at a
   placeholder IRAM base. Calls into the head are resolved to the head's fixed addresses; whatever the module needs from
   `libc` / `libm` / libraries that the head does not have is pulled into the module itself.
3. **Package** (`scripts/esp8266_modules.py`, post-link): per module the bytes and two lists of word offsets that hold
   an address of the module itself (flash or RAM / IRAM), the init array, the entry point, the head build id.

Why separately:

- the head never references a module by construction (the head is linked first, without them); the Beken tail needs a
  `--cref` check for the same thing because it is cut out of one link;
- template / inline copies (`std::vector<...>`, `String` helpers) that a module needs are in the module, never shared
  between two modules, so modules do not depend on each other;
- `--no-relax` only for the modules: with `-mlongcalls` every call from a module into the head stays `L32R` + `CALLX0`
  through a literal word (an absolute address), never a PC-relative `CALL0`, which would break when the module moves
  and could not reach the head from 1 MB away (`CALL0` reaches +-512 KB). The head keeps the relaxation (smaller).

`-mtext-section-literals` (the core's default) keeps each function's literals in its own section, so `L32R` inside a
module stays PC-relative to its own code and moves with it.

## Install: the board places the module itself

Any client uploads the same file: the WebUI, the head's own small page, a script on the LAN, `curl`. No step needs
GitHub or a browser with JavaScript.

1. `POST /modules/upload` (multipart or raw, Digest auth as `/upgrade`) with a module package, or with the whole
   container `espurna-esp8266-1m-modules.bin` plus `?names=sensors,webui` (the board takes only those parts).
2. The board checks the package header first (head build id, size against the free range, RAM / IRAM against the
   arenas), picks the slot (lowest module start minus the module size, 4 KB aligned) and the arena offsets.
3. It writes the module while it arrives, sector by sector, and adds the slot / arena bases to the words of the
   relocation lists on the way (the lists are sorted and come before the bytes they patch: a few hundred bytes of
   heap, no buffering of the module). CRC32 over the bytes as received and as written.
4. Then the directory entry (the commit), the answer, and one restart when the client says so (after the last module).

No TLS, no download on the board (BearSSL would need ~27 KB of heap). A Python copy of the same placing in the build
script is the reference for the spec test.

## Boot

`modulesSetup()` early in `setup()`, after the settings, before the modules' own setup would be:

- skip all in unstable mode (`systemCheck()`: the board then runs the head only, so a crashing module cannot lock it);
- per directory entry: build id = head's, CRC over the flash range; copy its initialized data (`.data` and `.rodata`)
  into the RAM arena, zero its `.bss`, copy its IRAM code into the IRAM arena, run its init array, call its entry with
  the head's registry;
- the entry calls the registration functions ESPurna already has: `sensor::add()` / driver load hook, relay providers,
  `espurnaRegisterLoop`, `terminalRegisterCommand`, `wsRegister`, `mqttRegister`, settings handlers.

Arenas: fixed static blocks of the head (`.bss` and an IRAM block), so their addresses are fixed for the build. Sizes from the measurements: RAM ~2 KB, IRAM ~1.5 KB (ISRs of HLW8012, PWM, IR receiver, rc-switch).

## Update of the head

1. WebUI: `POST /modules/off` - the board erases the directory sector and restarts: head only (also most free heap for
   the upload).
2. Upload the new head `.bin.gz` to `/upgrade` (fits by the budget above), restart. The board works (relays, web, MQTT)
   right away.
3. The WebUI installs the modules the board had (names from the directory read in step 1, kept as a setting too) from
   the same release, restart.

While modules are running, `/upgrade`, ArduinoOTA and the async OTA refuse to start (409, *modules installed: modules
off first*): the update would erase module code the board is running. In unstable mode no module runs, so OTA works
there directly.

From old firmware (image bigger than the budget): as today through the mini, which downloads the head; modules after.

## Without GitHub, or when something goes wrong

The head alone is always a working, reachable board: Wi-Fi (and its softAP fallback), relays, buttons, MQTT, the small
page, API, telnet, ArduinoOTA. Everything a module needs comes in through one upload to the board, from any machine on
the LAN; the files can be kept on a PC or on a local web server (`webFwUrl` already points the WebUI to another
`firmware.json`).

| What happened | What the board does | Way back |
|---------------|---------------------|----------|
| No internet / GitHub down | head and modules keep running; nothing on the board needs GitHub | the files from a PC: upload on the small page or the WebUI, or `firmware_esp_net.py` / `curl` to `/modules/upload` |
| Head update stopped half-way (no WebUI module yet) | new head running, modules erased | the small page or a script uploads the container (`?names=` of the old directory, kept in the settings) |
| Module upload broken off, power cut | no directory entry or a bad CRC: the module is ignored | upload again |
| Module crashes the board | after the crash count, unstable mode: head only, modules skipped | `MODULES.OFF` over telnet / API / small page removes them, or install a fixed one |
| Module of another build left on the board | build id differs: ignored, shown as *stale* on the small page | upload the container of the running head's release |
| Head itself broken | as today: unstable mode with Wi-Fi + ArduinoOTA, or its softAP, or the wire | `espota` with the head `.bin` |

Terminal / API commands of the head: `MODULES` (directory, free space, arenas, build id, stale entries), `MODULES.OFF`
(erase the directory, restart), `MODULES.REMOVE <name>`.

## Release and WebUI

- Release (one file per image): `espurna-esp8266-1m.bin` (the head) and `espurna-esp8266-1m-modules.bin`, a container
  of every module package with an index (like `espurna-beken-matter.bin`).
- Pages: the head `.bin.gz` and the container; `firmware.json` kind `modules` with the list (name, size, RAM / IRAM).
- WebUI: ADMIN lists installed / available modules with the space they take and what is left; *Add* / *Remove*
  (remove = rewrite the directory without it, restart; the space is reused at the next install or compacted by
  reinstalling all).
- The WebUI module of a release is the `all` preset (85 K; panels show only when their module reports in over the
  WebSocket, as today with the build flags). The head serves it straight from its flash slot. Without it the head's
  own small page answers on `/`.

## Risks / open questions

- **Head budget.** About 593 KB at most, estimated 577 KB (without the WebUI and the modules of the table, with API
  and telnet): 16 KB of margin for the head to grow, after that the next thing has to move out (NTP, UART, button
  types, Alexa is already out).
- **No WebUI right after a head update** until the browser has put the WebUI module back: the page that drives the
  update is already loaded and keeps going; a board left half-way has the head's small page.
- **Code that is not a module yet.** Head files call module code under `#if X_SUPPORT` (sensors: Domoticz, Home
  Assistant, Thingspeak, InfluxDB, Prometheus, RPN rules, thermostat, terminal). Each needs a hook through the registry;
  the link of the head without the module shows every such call (undefined reference).
- **IRAM**: about 2 KB free in a 1 MB image; ISRs of the installed modules have to fit the arena.
- **RAM**: module static data lives in the arena (taken from the heap region for good); module objects are created with
  `new` at the entry, as the sensor drivers are today.
- **Global constructors** of a module run from its own init array after the checks, never from the head's.
- **Size overhead**: template copies per module, calls through literals; module total a few % above the same code in a
  monolithic image (to measure).
- **Crash dumps**: an address in a module needs that module's ELF and slot base; the directory goes into the crash
  report.
- **Power loss**: during a module install - no directory entry or a bad CRC, the module is ignored; during the head
  update - as today (eboot).
- **4 MB boards** are out of scope (`esp8266-4m` stays monolithic).

## The head's own page: *Enable WebUI*

The head is as plain as the mini on the outside: its page on `/` (a few KB, no WebUI preset) shows the status, the
relays, a file upload and the button **Enable WebUI**. The button takes the WebUI module of the running release from
GitHub Pages in the browser and uploads it to `/modules/upload`; without internet the same module file is picked from
the disk. After the restart `/` serves the full WebUI from the module slot; its ADMIN lists and adds the other modules.

## Prototype status (2026-11-08): works on two Sonoff Basic

`code/platformio_modules.ini`: `esp8266-1m-head` and `esp8266-1m-mod-sensors` (`-e esp8266-1m-head -e
esp8266-1m-mod-sensors`, the head first). Files: `espurna/modules.h`, `espurna/modules_esp8266.cpp` (loader, upload,
`/modules`, the head page, `MODULES*`), `espurna/module_sensors.cpp`, `espurna/modules_exports.txt`,
`scripts/esp8266_module.ld`, `scripts/esp8266_module_link.py`, `scripts/esp8266_head_exports.py`,
`scripts/esp8266_data_module.py` (the WebUI module from `html/build/index.sensor.html.gz`); `main.cpp`, `web.cpp`,
`ota_web.cpp`, `ota_arduinoota.cpp` call the loader under `MODULES_SUPPORT`.

The head of the prototype: generic 1 MB image without the embedded WebUI, without scheduler, Home Assistant, Alexa,
Domoticz, Thingspeak and mDNS (off, **not yet modules**), with API and telnet.

| | Size | |
|-|-----:|-|
| head `.bin` / `.gz` | 598 736 / 409 659 | `ota_web` takes up to 421 888 behind it: **12 KB to spare, updates itself** |
| module `sensors` | 88 700 (package 94 404) | RAM 2 744 of 3 072 (data 1 948, bss 792), IRAM 384 of 1 024; 2 021 flash, 339 RAM, 7 IRAM relocations |
| module `webui` | 49 508 (package 52 908) | the `sensor` preset, served gzip from the slot |
| free for more modules | 278 528 | with both installed |
| heap at boot | 38 776 | 37 424 with the embedded WebUI and the integrations |

Tested on 10.0.5.0 (Sonoff Basic, from `esp8266-1m`) and 10.0.5.58 (Sonoff Basic, from ESPurna 1.15 through the mini,
settings kept):

- module install over `/modules/upload` (2-35 s), restart, `MODULES` *running*, the dummy sensor's magnitudes in
  `magnitudes`, reported over MQTT every minute, the full WebUI with the SENSORS panel (headless Chromium);
- **head update v1 -> v2 with one `/upgrade` of the `.gz`, no mini**: `MODULES.OFF`, upload 20 s, the board up with
  relays / MQTT on the head alone, the v2 modules installed again, running;
- `/upgrade` while a module runs: `409 modules installed, MODULES.OFF first`, nothing written;
- a package with one flipped byte: `400 package crc`, no directory entry;
- the v1 WebUI module on the v2 head: `400 the module was built for another head image`;
- `UNSTABLE`: the board in unstable mode (softAP only, the module not started), back by itself after the 10 minutes
  (`SYSTEM_CHECK_EXIT`) with the module running;
- `/` without the WebUI module: the head page (1.8 KB: status JSON, module file upload, *Enable WebUI*).

Found while linking:

- **Head exports.** `--gc-sections` drops head functions only a module uses (`wsPostSequence`, `settings::convert<double>`,
  `String::insert`, `pulseIn`, `twi_*`, `uartClaim` ...). The module link appends them to `modules_exports.txt`, the
  head keeps them (`-Wl,-u`), one more build of the head. 15 symbols for the sensors module.
- **No second copy of head code.** A library the head links (the Arduino core) or an object the head defines
  (`uart.cpp` of the generic image) never goes into a module: its members carry state (interrupt table, waveform /
  timer1, UART ports). The first link had `core_esp8266_wiring_digital`, `_waveform_pwm`, `_timer` and `uart.cpp` in
  the module. Now the module gets only what the head does not have (HLW8012, OneWire, Wire, `i2c.cpp`,
  `driver_onewire.cpp`, stateless `libm` / `libgcc` parts), and its entry runs a setup only when that code is its own
  (address inside the module).
- Two links at different bases need a **different delta per base**: with the same delta for RAM and IRAM every RAM
  word was taken for IRAM (caught before the first upload, the script now asserts it).

Not done yet:

- *Enable WebUI* from GitHub: the button is there, CI does not publish modules yet; ADMIN of the WebUI lists no
  modules;
- CI / release files, `fwinstall.mjs`;
- power cut during a module install (needs a hand on the plug);
- ArduinoOTA while a module runs erases the directory and goes on (the module code keeps running until the restart).

## Integrations as modules (2026-11-08, second step)

One env, `esp8266-1m-modules`, compiles the tree with every module's part on and links each line of `custom_modules`
(name | objects | objects only when the head has none | functions it provides). Entries are one macro each
(`ESPURNA_MODULE(scheduler, "scheduler", 50) { schSetup(); }`, `espurna/module_*.cpp`); the header has a `priority`,
the loader starts the providers (sensors 10, mdns 20) before their users (50).

**Modules call each other through the head.** Home Assistant, Domoticz and Thingspeak read magnitudes
(`magnitudeCount`, `magnitudeInfo`, `sensorWebSocketMagnitudes` ...), the sensors module pushes values into Domoticz
and Thingspeak (`domoticzSendMagnitude`, `tspkEnqueueMagnitude`). The head defines these 16 functions as forwarders
(`modules_api_esp8266.cpp`) to a slot of `espurna_module_api[]`; the provider's entry fills the slots; the link script
renames the provider's own definitions (`objcopy --redefine-syms`, `espmod_<symbol>`), so a call from another module
always lands in the head. Provider not installed: no magnitudes, nothing sent, nothing breaks.

**Library code: per member, not per library.** A library member the head links *and* that keeps state (`.data` /
`.bss`: interrupt table, timer1, the `MDNS` object) exists only in the head, what a module needs of it is exported by
the head. Every other member may come with the module (code the head dropped or never needed: `pulseIn`, I2C, the mDNS
logic, parts of the async web server); its calls into the head resolve to the head. With that the head needs only a
handful of exports and got smaller (604 -> 583 KB); a module may carry a copy of some head code (Alexa +4 KB).

**mDNS out of the head.** ArduinoOTA references `MDNS` although ESPurna starts it without; the head is linked with
`--wrap` stubs for its four methods (`scripts/esp8266_head_exports.py`), the `MDNS` object stays in the head (8 bytes
of state), LEAmDNS (~21 KB) is the `mdns` module's.

Windows: 55 `-Wl,-u` made the link command too long (`ld returned 5`); the exports go in as one implicit linker
script with `EXTERN()`.

| | Image | RAM | IRAM |
|-|------:|----:|-----:|
| head | 583 152 (`.gz` 398 413, **40 KB to spare** for its own update) | arenas 5 120 + 2 048 | |
| sensors | 94 168 | 2 916 | 1 088 |
| scheduler | 30 096 | 472 | |
| mdns | 25 432 | 192 | |
| alexa | 22 592 | 620 | |
| homeassistant | 14 928 | 304 | |
| domoticz | 8 052 | 192 | |
| thingspeak | 6 944 | 240 | |
| webui (data) | 49 512 | | |

10.0.5.0 (Sonoff Basic): the head updated itself from the previous one (one `/upgrade`), all eight modules installed
(1-5 s each), every one *running*, 168 KB still free, heap at boot 36.9 KB (10.7 KB available after 3 minutes).
Checked: `description.xml` of Alexa, an mDNS answer for `ESPURNA-074685.local` (unicast query, the PC is in another
subnet), the commands of the modules (`HA.SEND`, `SCHEDULE`, `MAGNITUDES`), reported magnitudes, the WebUI panels of
Domoticz / HASS / SCHEDULE / THINGSPEAK; the Domoticz panel lists the relays of the head and the magnitudes of the
sensors module. Not checked against real servers (Home Assistant, Domoticz, Thingspeak are off on the bench board).

## The modules page, light and RF, the guard (2026-11-08, third step)

**Page.** The head serves `/modules.html` (also on `/` while no WebUI module runs; the WebUI module has a MODULES menu
entry pointing there): bars of the flash (head | modules | free), the RAM arena (with the free heap now and after the
boot) and the IRAM (TCM) arena, yellow at 85 % taken ("almost full"); one switch per module with what it takes of
each, *fits, tight* in yellow, *no RAM / flash / IRAM for it* in red. On: the module's package is cut out of the modules
file of the release (`espurna-esp8266-1m-modules.bin`, EMC1: count + 48-byte entries + the packages; written by the link
script, from the disk or from the release list) and uploaded to `/modules/upload`; off: `/modules/remove`; *Restart*
applies. `/modules` (JSON) has the sums and the largest gaps.

**Placing: best fit.** A removed module leaves a gap in the flash and in the arenas; the smallest gap that holds the
next one is taken (first fit had put a 240-byte module into the 304-byte hole of another, which then did not fit
anywhere).

**Light and RF modules.** `light` (light.cpp, pwm.cpp, my92xx; 40 KB, RAM 1.5 KB) provides 33 functions through the
head (Home Assistant, Alexa, Domoticz and the scheduler use them); PWM itself is `analogWrite` of the core, whose timer
interrupt stays in the head. `rf` (rfbridge.cpp, rc-switch; 16 KB, RAM 1.3 KB, IRAM 776 for the receiver interrupt).
Every module at once needs 8 KB of RAM, the arena has 5 KB: the page shows which ones do not fit. On 10.0.5.0: WebUI,
light, RF, Home Assistant, Alexa, scheduler running, heap 10-12 KB.

**Guard** (the reason in the module directory sector; the modules do not start while it is there; the page shows it in
red with *Start the modules again*, terminal `MODULES.ON`):

- free heap below 2 KB for 5 s with module code running -> `modulesOff`, restart;
- 3 boots in a row crashed (exception, watchdog) within the first 60 s with the modules running -> `modulesOff`.
  Counted in `.noinit` RAM, which a crash restart keeps. ESPurna's unstable mode (3 crashes within 60 s) comes at the
  same boot; after its 10 minutes the board starts without the modules.

Tested with `MODULES.TEST.LOWHEAP` (the guard sees no heap) on 10.0.5.0: off, restart, the page with the reason, the
button, all modules running again.

**Found: the directory was in the settings' rotation pool.** EEPROM_Rotate keeps the settings in sector 251 and
rotates through the sectors under it (250 on a 1 MB chip, 8 sectors on 4 MB). The directory at 0xFA000 (sector 250)
was erased by the first settings write after it (the guard's `modulesOff`), and a directory write invalidated a
settings copy (EEPROM_Rotate took the other one, nothing lost). The directory is now the sector under the pool,
computed at runtime from EEPROM_Rotate (`base() - size()`): 0xF9000 on 1 MB.

**RAM of a module in IRAM** (the ESP8266 counterpart of the Beken TCM move). The head runs the core's 48 KB IRAM
layout (`PIO_FRAMEWORK_ARDUINO_MMU_CACHE16_IRAM48`, flash cache 32 -> 16 KB) with `NON32XFER_HANDLER`; the IRAM after
its code (17.5 KB) holds the `.data` / `.rodata` / `.bss` of the modules installed with `?ram=iram` (a DRAM / IRAM
choice per module on the page, a fourth bar *RAM in IRAM*). That memory takes 32-bit access only: the loader copies
with word stores, byte and half-word access of the module goes through the core's exception handler (slower; not from
an interrupt, so modules with their own interrupt code should keep DRAM). The handler costs ~2.5 KB of DRAM `.bss`.
On 10.0.5.0 the sensors module (2.9 KB RAM, did not fit the DRAM arena next to light, RF, HA, Alexa, scheduler) runs
from IRAM: magnitudes read and reported, no crash; heap 6.2 KB with the seven modules; the WebUI (63 KB) loads in
0.6 s.

**Guard, boot crashes, second try.** ESPurna's own unstable mode came first (its counter also takes the plain restart
before the crashes): in unstable mode no module runs, and that boot had reset the guard's count, so after the 10
minutes the modules started (and would crash) again. Now only modules that ran past their first 60 s clear the count;
boots without module code (the unstable mode, its exit restart) keep it.

**Guard, where the decision lives.** First it was the setting `modulesOff`: saved later from the loop, a crash before
that lost it (fixed with an immediate commit), and after the crashes and the 10 minutes of unstable mode EEPROM_Rotate
came back with an older copy of the settings without the key (37 keys, as before the guard), and the modules started
again. The reason is now a field of the module directory (its own sector, outside the rotation, one erase + write).
Tested on 10.0.5.0 (`MODULES.TEST.CRASH` at 5-15 s after each boot with webui, Home Assistant, Alexa, scheduler and
the sensors in IRAM running): crash 1, crash 2, ESPurna's unstable mode for 10 minutes, back with the modules, crash 3
-> *the boot crashed 3 times in a row with the modules running*, the board up at once; one more crash after that and
14 minutes later the decision was still there; *Start the modules again* -> all running.

## The MODULES panel of the WebUI (2026-11-08, fourth step)

The WebUI module (preset `modules`: `MODULE_MOD`, sensors, light, RF bridge) has a MODULES menu entry and panel
(`html/src/panel-mod.html`, `html/src/modules.mjs`, tests `html/spec/modules.spec.mjs`). It asks the same `/modules` and
uses the same endpoints and modules file as the head's page, which stays for boards without the WebUI module.

- **Memory**: four meters (flash with the head, RAM arena, IRAM code arena, RAM in IRAM) with the share and what takes
  it; yellow from 85 % (*almost full*), red text when a module would not fit. Pointing at a module that is off adds a
  striped part to each meter: what that module would take.
- **Modules**: a card per module (installed or in the modules file) with an ON/OFF switch, a line on what it brings,
  chips for flash / RAM / IRAM (red when that part is short), and DRAM / IRAM for its RAM before it is installed. The
  state: *running*, *after restart*, *off after restart*, *can be added*, *fits, tight*, *does not fit*, or the guard's
  and loader's own states. Changes waiting for a restart show a yellow bar with *Restart now*.
- **Source**: the modules file from the disk or *From GitHub* (the release of the running commit in `firmware.json`,
  file kind `modules`).
- **Guard**: when the modules are off the panel says why and offers *Start the modules again*.
- `/modules` is asked only when the panel is opened: with webui, Home Assistant, Alexa, scheduler and sensors running the
  heap is 4-10 KB, and the first page load (page + WebSocket dump) needs all of it. Two browser tabs loading the WebUI at
  once restarted 10.0.5.0 (no crash dump: the terminal does not answer at that heap); one tab at a time works.
- Size: the WebUI module grew from 63.0 to 68.5 KB (gzip). Other presets get `MODULE_MOD = false` (nothing changes in
  them); `'mod'` is left out of the `all` preset.
- `scripts/esp8266_module_link.py` now depends on `html/build/index.modules.html.gz`, so a new WebUI build reaches the
  modules file.

Tested on 10.0.5.0 (head v13 unchanged, md5 4b2becc1): panel at 1100 px and 380 px wide, the hover preview, ON for
thingspeak (*after restart*, the restart bar), OFF again (gone at once, it never ran).

## Two ESP8266 images: initial and full

The user's target (2026-11-08): the ESP8266 releases shrink to two files.

- **initial**: the head. Serial or OTA (through the mini from old firmware) on 1 MB boards; the WebUI switches add and
  remove modules.
- **full**: the head with every module and the WebUI already placed (the build script places them itself, the Python
  copy of the board's placing) and its directory: one file for serial at `0x0`, for 4 MB boards or anything that
  should have everything. The relocation maps of its modules follow the image in the file (behind 1 MB, never
  flashed), so the browser can cut any module out of `full` and upload it to a 1 MB board, which places it as now.

Open points: everything on at once has the RAM problem of the old `esp8266-4m` (a switch must keep a module from
starting, the module `.rodata` should go to flash); 4 MB boards on the 4 MB layout keep their settings at the end of
the chip, the head needs a one-time move of them; head + all modules is ~950-990 KB, so `full` would just fit 1 MB as
well, but leaves no room for an update there.

**Built (2026-11-08).** The modules env writes both next to the modules file
(`scripts/esp8266_full_image.py`, called at the end of `scripts/esp8266_module_link.py`):

| File | What | Size |
|------|------|------|
| `espurna-esp8266-1m-initial.bin` / `.bin.gz` | the head | 595.6 KB / 405.5 KB |
| `espurna-esp8266-1m-full.bin` | flash image from 0x0: head, 10 modules, directory, then the modules file | 1.41 MB |
| `espurna-esp8266-1m-modules.bin` | the modules file alone (also inside `full`) | 359 KB |

`full`, byte by byte:

- `0x0`: the head (the same bytes as `initial`);
- under the directory: every module of the modules file, placed and relocated the way the board places an upload (the
  Python copy of `prepare` / `place` / `finish`: best fit, flash from the top, arenas from the bottom, the same deltas,
  the same CRC). Order and RAM: the modules file order; `custom_full_ram_in_iram` (ini) names the modules whose RAM goes
  to the IRAM tail, now `sensors` (the others need 5104 of the 5120 bytes of the DRAM arena; light and RF have interrupt
  code and stay in DRAM);
- `0xF9000`: the directory (`EDR2`, 10 entries, no guard reason);
- `0xFA000-0xFFFFF`: 0xFF (settings and SDK sectors start empty: a fresh install);
- `0x100000`: the modules file (EMC1). On a 4 MB chip it lands in flash the 1 MB layout does not use; the head's page and
  the MODULES panel take an image file with `EMC1` at 1 MB as a modules file, *From GitHub* takes the file kind `full`
  when there is no `modules`.

Flash `full` with the header's flash size kept (esptool `write_flash` default `keep`): the head finds its settings and
the directory from the 1 MB header. Free in `full`: 68 KB flash, 16 bytes of the RAM arena, 184 bytes of IRAM,
14.2 KB of RAM in IRAM. The build checks each placed module the way the loader does at boot (magic, own address, head
MD5) and a separate reader of the file checked the directory and every CRC.

Tested on 10.0.5.58 (head v9, no modules): `initial.bin.gz` over the network (the first upload was cut at 68 KB by the
board, the second went through; md5 f8edbb54), then all 10 modules switched on in the head's page from the **full**
file, in its order, sensors RAM in IRAM: the board placed every module at the same offset as `full` (0xE2000 sensors
... 0xA3000 webui), RAM arena 5104 / 5120, IRAM 1864 / 2048, 69,632 bytes of flash free, the same as the build said.
After the restart all 10 run, no guard decision in 4 minutes; free heap 4-9 KB. With that heap the WebUI took several
tries (about 2.5 minutes) to load and `/modules` once answered empty: everything on is usable, but it is the limit,
not a configuration for a board people use. The serial path of `full` (a 4 MB board flashed at 0x0) is not tested:
there is no 4 MB bench board.

## The RAM choice on 4 MB boards and on running modules (2026-11-09, sixth step)

User: «для 4 мб тоже нужен выбор» — `full` fixes the RAM of every module (sensors in IRAM), and the panel offered the
DRAM / IRAM choice only for a module that is off.

- **The board's own modules file.** A 4 MB board flashed with `full` carries the modules file at 1 MB. The head reports
  the real chip size (`chip`) and the size of that file (`file`, 0 when there is none) in `/modules` and serves it at
  `GET /modules/file`; the SDK refuses reads past the size of the image header (1 MB), so `flashchip->chip_size` is
  raised to the real size for each read. The MODULES panel and the head's page load it by themselves when `file` is
  not 0: on such a board the switches and the RAM choice work without picking a file or going to GitHub.
- **Moving the RAM of an installed module.** Its card has the DRAM / IRAM select too. A change uploads a new copy of
  the module with `/modules/upload?replace=1[&ram=iram]`: the head places it next to the old copy, writes and checks it,
  and only then swaps the directory entry (a failed upload leaves the old module as it was). The old copy's RAM and IRAM
  count as free for the new one (the arenas are filled at boot), its flash does not. One restart applies it. The
  select is there when a second copy fits: a free flash gap of the module's size and room for its RAM in the other
  memory (sensors needs 92 KB of free flash; with all 10 modules there is 68 KB, then it is off, restart, on).
- **Running code keeps its flash until the restart.** The head remembers the flash of every module started in this
  boot; a module switched off (or replaced) keeps running from it, so nothing new is placed there before the restart.
  Before, switching sensors off and on again could put the new copy over the running code.
- `/modules` states are matched by flash offset and CRC, not by the directory index (a removal moved the indexes and a
  new module showed the state of another one).
- The panel's message line got its own class: a card's "Not enough RAM" line took the panel's messages, so an upload
  error was written into the light card and the panel looked stuck.

Tested on 10.0.5.0 (head f6a789d7, sensors + scheduler + alexa + homeassistant + webui): in the WebUI panel the sensors
select DRAM → IRAM wrote the new copy at 0xA7000 while the old one ran at 0xE2000 (free flash 82 KB until the
restart, 94 KB after); after the restart sensors ran with its RAM in IRAM (2916 bytes), then IRAM → DRAM put it back at
0xE2000, restart, all five running, heap 7 KB. The first try had failed with "not enough IRAM in the arena" (the old
copy's 1088 bytes of IRAM code were counted) and the panel had shown nothing (the message went into the light card):
both fixed as above.

The 4 MB path (`chip` > 1 MB, `file` from flash past the header size) is not tested on hardware: both bench Sonoff
Basics have 1 MB chips (`chip` 1048576, `file` 0, `/modules/file` 404 there).

## ESP32 and Beken: Matter and NSPanel as modules (plan)

User (2026-11-09): the ESP32 and Beken images get modules too, Matter and the NSPanel among them.

Why there:

- **Beken BK7231N (CB3S, 2 MB):** the Matter image is 1.40 MB, the app partition 1124 KB: today Matter does not fit a
  2 MB board at all. A head without Matter plus a Matter module in the space of the OTA download partition (664 KB,
  free once the head updates itself the ESP8266 way) is the only way to get Matter there.
- **ESP32 (4 MB, app0 2880 KB):** `esp32-matter` (2.14 MB) and `esp32-matter-nspanel` (2.54 MB) become one head plus
  Matter and NSPanel modules: one ESP32 image instead of two, and the 1.2 MB of Matter only on boards that use it.

The ESP8266 scheme carries over: the module links against the head's ELF (its symbols, the head forwarders), the
board relocates it on install for the address it runs at, RAM comes from arenas of the head, a directory lists what is
installed, the WebUI panel is the same (`modules.mjs` reads `/modules`). What differs:

| | ESP8266 | ESP32 | BK7231N |
|---|---|---|---|
| Code runs from | flash mapped 1:1 at 0x40200000 | flash pages mapped by the MMU (`esp_partition_mmap`, instruction and data maps) | flash XIP, stored with a CRC16 per 32 bytes and encrypted with the chip key |
| Module address | fixed by its flash offset | the mapped address: map the modules partition first thing at boot (the same address for a given head), relocate on install for it, check it at boot and relocate again from the package if it moved | fixed by its flash offset; the board must write the module in the CRC + encryption format the flash controller reads (as the bootloader does with the .rbl) |
| CPU | Xtensa LX106 | Xtensa LX6 (`-mlongcalls`, literals next to the code) | ARM968 (ARMv5TE, Thumb) |
| RAM of a module | DRAM arena / IRAM tail | an arena in the head, or one allocation at boot (the same address for a given head, checked like the code map) | an arena in the head |
| Space | 1 MB chip, under the directory | a `modules` partition (app0 shrinks once Matter leaves the head) | the download partition (664 KB on CB3S), the head updates without it |

Matter as a module is the big one: the CHIP stack runs its own FreeRTOS tasks and needs lwIP, mbedTLS, NVS and (on
ESP32) the IDF event loop from the head; all of that stays in the head, the module is CHIP + the ESPurna Matter glue.
Its start, stop and quiet mode (WS closed, MQTT off while commissioning) are the head's hooks that the module fills.
The NSPanel module is the panel's UART protocol, its pages and the TFT upload; the head keeps the relays, the
thermostat logic and the settings.

Order: ESP32 first (lab board 10.0.5.66, an NSPanel to try on after it), NSPanel before Matter (smaller, no tasks of its
own); then Beken, where the flash format is the first thing to prove: write a small module in the CRC + encrypted
format and call it.

## Prototype steps

1. Measurements (this note).
2. Head without the WebUI and the module code, one sensors module (manager + all drivers of `-1m-sensor`), the WebUI
   data module, the package script, the real head size against the budget.
3. Loader in the head (`modules.cpp`), `/modules` endpoints, OTA guard, unstable-mode skip.
4. Placing on upload in the head, its Python reference, `MODULES*` commands; the WebUI / small page buttons over the
   same `/modules/upload`.
5. Bench test on two Sonoff Minis (not in walls): head install through the mini, module install, a magnitude on WS /
   MQTT, a head update with module reinstall, a module with a broken CRC, power cut during a module install.
6. Then split further (light, IR, RF, relay MCUs, integrations) and the CI / release changes.
