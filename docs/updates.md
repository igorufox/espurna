# Firmware updates and releases

How this fork builds its images, where the WebUI finds them and how an update reaches the board. The install itself
(head + modules from the `.ota`, the Apply) is one procedure for every chip: [docs/modules.md](modules.md). This page
has the releases, CI and what each chip's hardware does below that procedure.

## Releases

Every push to `esp32-port` (and every `esp32-*` tag, or a manual run) is built by
[`.github/workflows/esp32.yml`](../.github/workflows/esp32.yml) and becomes **one release** with the images of all chips.
Its tag is `esp32-YYMMDD-<sha7>` (the chip prefix is historical, the release holds every chip). Automatic builds are
preview releases (pre-releases, the newest 5 kept); a preview tested on hardware becomes a stable release by a manual
run of the workflow with `promote` = its tag. Releases of hand-made `esp32-*` tags stay.

| Build (matrix) | Release files | Image name (`ESPURNA_IMAGE`) |
|----------------|---------------|------------------------------|
| `esp8266` of `platformio_esp8266.ini` | `espurna-esp8266.bin` (initial image), `espurna-esp8266.ota` | `esp8266` |
| `esp32` of `platformio_esp32.ini` | `espurna-esp32.bin` (merged initial image), `espurna-esp32.ota` | `esp32` |
| `beken` of `platformio_beken.ini` | `espurna-beken-tuya.uf2`, `espurna-beken-quectel.uf2` (initial images), `espurna-beken.ota` | `beken-tuya` / `beken-quectel` (by the bootloader found at boot) |
| `code/migration-app` | `espurna-esp8266-1m-migration.bin` | `esp8266-1m-migration` |

One env per chip: the head, every module and these files are one build ([modules.md](modules.md#building)). What the
initial image and the install image (`.ota`) are: [modules.md](modules.md#release-files).

Boards still on an image of the older releases (`esp8266-1m`, `esp8266-1m-light`, `esp8266-1m-sensor`, `esp32-matter`,
`esp32-matter-nspanel`, `beken-matter`) find the head of their chip under their own image name in `firmware.json`;
`esp8266-4m` (settings at the end of 4 MB) does not, its settings move first ([modules.md](modules.md#esp8266)).

The firmware knows the image it was built as (`ESPURNA_IMAGE`, `fwImage` in the WebUI) and its version carries the git
revision (`26.11.0-git<sha>`, set by [`scripts/pio_main.py`](../code/scripts/pio_main.py) on every chip). The WebUI
finds the running release by that revision and lists only the files of the running image.

### firmware.json on GitHub Pages

The `pages` job copies the kept releases to GitHub Pages and writes two lists:
[`firmware.json`](https://igorufox.github.io/espurna/firmware.json) (the stable releases, the default list of the WebUI)
and `firmware-preview.json` (stable and preview). Per release: the tag, channel, date, commit and, per image, its files
with kind (`install` for the `.ota`, `ota`, `serial`, `safeboot`, `app`), size and SHA-256. Besides the release files the
folder of a release on Pages has the copies the boards need: the `.bin.gz` of the ESP8266 images, and of the ESP32 image
its `-safeboot.bin` and `-app.bin`, cut out of the merged `.bin` along its partition table. The release assets themselves
cannot be used by the update code: GitHub serves them without CORS (the browser may not read them) and only with ECDHE
ciphers (the Beken mbedTLS has none). Pages sends `Access-Control-Allow-Origin: *`.

Another list: the `webFwUrl` setting (*ADMIN*, OTA, *Release list*), e.g. `firmware-preview.json` for previews; empty is
the default URL above. The ESP32 safeboot and the ESP8266 migration image read the same setting from the ESPurna settings.

### CI notes

- **Python 3.13**, not `3.x`: 3.15 broke the build (pioarduino takes 3.10 to 3.14, LibreTiny could not make its venv).
  The PlatformIO cache key has `py313` in it, the caches of the 3.15 runs hold a broken LibreTiny venv.
- PlatformIO Core 6.2 or newer (pioarduino needs it).
- ESP32: `pio run -d safeboot-app` runs first, `scripts/esp32_merge.py` puts the safeboot into the factory partition of the
  merged `.bin`.
- The migration image `.bin.gz` (made for Pages) is `pigz -11` (zopfli), about 4 KB smaller than `gzip -9`: it has to fit behind the
  biggest 1 MB image of the older releases. The other `.bin.gz` files are `gzip -9 -n`.
- The Pages artifact is named per run attempt (`github-pages-<attempt>`): a re-run otherwise finds the artifact of the
  failed attempt as well and `deploy-pages` stops.
- **The WebUI** is the `webui` module, packed from `code/html/build/index.html.gz`, the output of `npx gulp build` in
  `code/` (`scripts/module_link.py`, `webui_page`): a chip build that does not find it has no `webui` package.
- Beken: the Matter library is not built here. It is a release asset of
  [igorufox/matter-bk7231n](https://github.com/igorufox/matter-bk7231n), see [below](#beken-matter-library).

## Updating from the WebUI

*ADMIN*, OTA section:

- **Upgrade**: a file from this computer (*Browse*, *Upgrade*).
- **From GitHub**, *Show releases*: the kept releases with the files of this board's image. *Install* downloads the
  release's `.ota` into the browser, checks its SHA-256 and installs it as on the MODULES page: the head when the board
  runs another build, then the modules the board has on ([modules.md](modules.md#updating-the-head)). The board restarts
  on its own during an install: keep it powered and keep the page open until the last step. The other files of a
  release (initial images for the wire) are download links.

What each chip does with the head, below that procedure:

### ESP32: the safeboot

The ESP32 images use the safeboot flash layout ([`code/esp32_4mb_safeboot.csv`](../code/esp32_4mb_safeboot.csv), the
offsets of Tasmota): the ESPurna safeboot ([`code/safeboot-app`](../code/safeboot-app), plain ESP-IDF) in the factory
partition at `0x10000` (832 KB), one 2880 KB app slot at `0xE0000` for the head and, in its tail, the modules. ESPurna
cannot overwrite itself, so `/upgrade` answers with an error and every head update goes through the safeboot: the WebUI
sends `/safeboot/switch`, the board restarts into the safeboot, the browser uploads the head to its `/upload`, the
safeboot writes app0 and boots ESPurna.

The merged `espurna-esp32.bin` (bootloader, partition table, safeboot, ESPurna) is the file for the first serial flash
at `0x0`; the safeboot takes it as well and writes only its ESPurna part.

**The safeboot itself**: a web page on port 80 (user `admin`, the ESPurna admin password): the releases of the running
image (from `firmware.json`, read by the browser), an upload of an image (the merged `.bin`, or an application image)
into app0, a download from a URL over https (ESP-IDF certificate bundle, SHA-256 checked). It joins Wi-Fi with the
ESPurna settings (`ssid0`/`pass0`, `ssid1`/`pass1`, static IP, hostname); without Wi-Fi in 30 s it opens its own access
point `<hostname>-SAFEBOOT` (WPA2, admin password) at 192.168.4.1. The next boot is set back to app0 right at its start,
so a crash or a power cut in the safeboot returns to ESPurna; after 15 minutes without activity it boots ESPurna as well.
Other ways in: the `safeboot` terminal command, the `/safeboot` page of ESPurna, and the crash-loop guard (unstable
boots in a row, `SAFEBOOT_CRASH_LOOP_MAX`, switch to the safeboot). ESPurna switches only into the ESPurna safeboot: a
factory partition with anything else (Tasmota's safeboot, nothing) is refused.

The `/safeboot` page of ESPurna also installs a safeboot (`/safeboot/install`, the safeboot part of a merged `.bin`) and
migrates **two-slot boards** (older ESP32 builds, `min_spiffs.csv` / `default.csv`) over Wi-Fi (`/safeboot/migrate`):
ESPurna writes the safeboot over the first slot, replaces the partition table and erases otadata, the board restarts
into the safeboot, which takes ESPurna into the new app0. NVS keeps its place, so the settings stay; ESPurna has to run
from the second slot for that. The NSPanel's file system (LittleFS with `nspanel.json`, 128 KB at `0x3D0000`) lies
behind the new app0 and keeps its place. The WebUI's release list no longer replaces a foreign safeboot (Tasmota's on an
NSPanel) by itself.

### Beken: the .uf2 of the board's bootloader

One head, one initial image per bootloader: `espurna-beken-tuya.uf2` (Tuya bootloader) and `espurna-beken-quectel.uf2`
(Quectel bootloader, `code/scripts/beken_quectel_image.py`); the `.ota` carries both heads. The board knows its
bootloader from the CRC32 of the first `0x11000` bytes of flash (Tuya `f684a1dc`, Quectel `64b02979`) and names its image
after it (`fwImage`: `beken-tuya` / `beken-quectel`), so the WebUI takes the right head. The board cannot download from
GitHub (no ECDHE in its mbedTLS): the browser always brings the file.

Each UF2 carries the partition table of its bootloader (tag `FAL_PTABLE`) and LibreTiny's Update writes the `download`
partition of that table, not of the one the firmware was built with. Before anything is written the board compares the
`download` partition of the file with its own bootloader's and refuses the other file
(`code/espurna/arch/beken/ota_beken.cpp`). An unknown bootloader gets no OTA at all. The update lands in the `download`
partition, the module area: one upload with the modules off, the bootloader installs it at the restart.

### ESP8266: upload; the migration image for older 1 MB images

The ESP8266 writes an update behind the running image and copies it over at the next boot (eboot unpacks `.bin.gz`). The
head is small enough for that with the modules off: one upload to `/upgrade`, no migration image.

Boards on a full image of the older releases (`esp8266-1m`, `-1m-light`, `-1m-sensor`) have about 200 KB free behind
it, too little for the head. Their own WebUI goes through the migration image ([`code/migration-app`](../code/migration-app)):

1. `POST /ota/fetch` stores the URL and SHA-256 of the `.bin.gz` in the settings (`otaFetchUrl`, `otaFetchSha256`).
2. The browser uploads `espurna-esp8266-1m-migration.bin.gz` to `/upgrade`, the board restarts into the migration image.
3. The migration image reads the ESPurna settings in place (Wi-Fi, static IP, hostname, `adminPass`, `webFwUrl`), takes the stored
   request once, downloads the `.bin.gz` (https), checks the SHA-256, writes it and restarts into it.

The migration image keeps its own page on port 80 after a failure, or when it has no request: status, last result, the releases
with the 1 MB images that fit, a file upload. Without Wi-Fi in 30 s it opens `<hostname>-MIGRATION` (WPA2, admin password) at
192.168.4.1, and tries the Wi-Fi again every 5 minutes. Its log is on the TX pin at 115200. The `.bin.gz` files are on
GitHub Pages next to `firmware.json`, not in the release: `https://igorufox.github.io/espurna/<tag>/espurna-<image>.bin.gz`.

**From ESPurna 1.15.** Old boards (1.15, core 2.7.4) usually have about 10 KB free: a web upload drops half-way, and 1.15
then keeps the update 'in progress' until the next restart (`reset`). Its terminal takes commands as the WebSocket action
`dbgcmd`, not `cmd`. What worked on a Sonoff Basic without a wire: back up the keys, `reset`, after 70 s put the migration image on it
with ArduinoOTA (`espota`), then the image on the migration image page. The settings migrate at the first boot (Wi-Fi, hostname and
password stay); apply the board template afterwards.

**From a board build to a generic image.** The generic images take the pins from the settings. Apply the board's
template (*ADMIN*, *Device Template*) before the update (it writes the values the board build had, nothing changes) or
right after the first boot.

**Memory for the WebUI and uploads (ESP8266).** About 40 KB of heap region; the settings buffer (4 KB), Wi-Fi and the web
server leave 10-20 KB:

- **lwIP 'Lower Memory'** (`PIO_FRAMEWORK_ARDUINO_LWIP2_LOW_MEMORY` in `platformio_esp8266.ini`): MSS 536, 2 KB receive
  window and 1 KB send buffer per connection. With 'Higher Bandwidth' (MSS 1460) the page load and an upload ran the heap
  out: truncated index (`ERR_CONTENT_LENGTH_MISMATCH`), uploads dropped at 41-63 %, abort in
  `AsyncWebSocket::makeBuffer`. The price is speed: 825 KB take 30-45 s.
- **WebSocket sends wait for the heap.** The queued WebUI data is sent only while the largest free block holds its JSON
  buffer plus 1.5 KB, and a single message is dropped (debug line) rather than allocated without room.
- To find what takes memory at boot, build with `-DSETUP_HEAP_PROBE_SUPPORT=1`: the terminal command `HEAP.SETUP` prints
  the free heap after every setup step and the difference to the one before.

## TLS and integrity

| Who downloads | TLS | Integrity |
|---------------|-----|-----------|
| Browser (all chips) | validated https to GitHub Pages | SHA-256 of `firmware.json`; the CRC32 of every part of the `.ota` |
| The board, a module package | none, the browser uploads | CRC32 of the package and of what was written; the head id |
| ESP32 safeboot (download from a URL) | ESP-IDF certificate bundle | SHA-256, checked before ESPurna boots it |
| ESP8266 migration image | BearSSL, TLS 1.2, RSA key exchange + AES-128-GCM, **no certificate check** (no CA store, no clock) | SHA-256 required, taken by the browser from `firmware.json` over validated https |
| Beken | none, the browser uploads | the UF2's partition table must be the board's |

## Beken Matter library

`libMatterApp.a` (connectedhomeip for the BK7231N) is built in [igorufox/matter-bk7231n](https://github.com/igorufox/matter-bk7231n)
by its own GitHub Actions workflow (`release.yml`): `tools/ci_build.sh` on Ubuntu 24.04 with the Linux build of the gcc
LibreTiny uses (`toolchain-gccarmnoneeabi@1.100301.220327`, GCC 10.3.1) and pinned connectedhomeip, LibreTiny 1.14.0 and
BDK packages, after every push to `main` that changes `gn/`, `lwip-override/`, `tools/` or the workflow. Each build is a
release `libmatter-<yyyymmdd>-<sha7>` with `matter-bk7231n-lib.zip` (`lib/libMatterApp.a` + `include/lwipopts.h`) and its
`.sha256`; the newest 5 are kept.

`platformio_beken.ini` pins one of them: `custom_matter_lib = <zip URL>` with `custom_matter_lib_sha256`
([`scripts/beken_matter_lib.py`](../code/scripts/beken_matter_lib.py) checks the zip and unpacks it into
`.pio/matter-lib/<hash>`). A new library release reaches the firmware when those two lines are changed in a commit of
espurna, so the order is: release the library, then change the two lines. `custom_matter_lib = latest` (the newest
release through the GitHub API) is still understood, for trying a release out; a local build:
`ESPURNA_MATTER_LIB_DIR=<folder>`. lwIP of the head is built with the library's `lwipopts.h`; the library itself goes
into the `matter` module only.

## Known limits

- **First page load right after a restart**: once on a Sonoff Basic the page loaded but the *ADMIN* menu stayed hidden
  (WebSocket data incomplete); four minutes later the same took 2 s. Cause not found.
- **Migration image TLS** does not validate the certificate; the SHA-256 from `firmware.json` is what protects the image.
- **Older ESP8266 `-1m-light` boards**: the migration image does not fit behind the 810 KB light image (about 6 KB missing): put the
  migration image on it with ArduinoOTA or over serial.
- **ESP32 boards with a foreign safeboot** (Tasmota's, on an NSPanel installed over Tasmota): the release list does not
  replace it; install ours from the `/safeboot` page of ESPurna.
