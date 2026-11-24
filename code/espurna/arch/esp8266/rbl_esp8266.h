/*

OTA OF THE HEAD, ESP8266 on ESP8266_RTOS_SDK: the RBL package (owner 2026-11-18: «по схеме tuya в загрузчике»)

One file for the two sides of it, C (the SDK's bootloader, bootloader/espurna_rbl.c) and C++ (the head, ota_esp8266.cpp):

- the head takes the RBL of a new head (espurna-esp8266.ota, scripts/esp8266_module_link.py) while the modules are off,
  writes the package right under the header sector and the 96 bytes of the header last
- the bootloader finds the header at its fixed place, checks the package before it erases anything, then SWAPS the heads
  a sector at a time (owner 20.11 «поменять местами»): each sector of the running head is deflated into the free part of
  the package's zone, then the sector of the new head is inflated in its place. The old head stays there, compressed in
  the same chunks, until the new one is confirmed: the new head erases the header sector after 60 s with Wi-Fi
  (ota_esp8266.cpp). Three starts without that and the bootloader puts the old head back. The progress is a bit per
  sector in the header sector, a power cut resumes at that sector. One bootloader on every chip size: the zone is the
  space under the header on 1 MB, the top 512 KB on 2 MB and more (ESPURNA_RBL_TOP_ZONE). A dry run first: when the old
  head would not fit the zone compressed, the new head is installed without a rollback, as before

The header is the RBL of the Beken bootloaders, one for both chips (libs/rbl.h), 96 bytes:
"RBL\0", encryption 0, compression 2 = the head a sector (4 KB) at a time, each sector a chunk of its own raw deflate
(zlib wbits -12): u16 length, the deflate bytes, padding to 4 bytes (scripts/esp8266_rbl.py), the build time, the name "esp8266", the version, sn (unused), the CRC32 of the package, the
FNV-1a of the inflated head, the size of the inflated head, the size of the package, the CRC32 of the 92 bytes before

Flash (arch/esp8266/partitions.csv, modules_esp8266.cpp): the head at 0x10000 grows up, the modules' directory is in
0xF8000 / 0xF9000; the header sector 0xF7000 is under it, the package under the header. The new head must end under the
package (no overlapping inflate), the package must start above the running head (it is written while that one runs)

*/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* the RBL header of every chip: espurna_rbl_t, its CRC32 and FNV-1a */
#include "../../libs/rbl.h"

#define ESPURNA_RBL_SECTOR      0x1000u
#define ESPURNA_RBL_HEADER_AT   0xF7000u
#define ESPURNA_RBL_HEAD_AT     0x10000u
#define ESPURNA_RBL_NAME        "esp8266"
#define ESPURNA_RBL_SECTORS     2u      /* compression: a raw deflate chunk per 4 KB sector */
/* compression: the same chunks, an image of the flash from 0x0 - another firmware (ESPurna 1.15 / 1.22 of the Arduino
   core with its eboot, owner 21.11: «пиши 1.15 по воздуху»). No swap and no rollback: the SDK's bootloader is
   overwritten too. A bootloader without this kind refuses it ("RBL of another kind"), the head stays */
#define ESPURNA_RBL_FLASH       3u

#define ESPURNA_RBL_TOP_TAIL    0x10000u        /* the last 64 KB of the chip: the SDK's system parameters */
#define ESPURNA_RBL_TOP_ZONE    0x80000u        /* 512 KB under it for the package, the data modules below */

/* the swap state, the package place, the header check: the same on every chip */
#include "../../libs/rbl_layout.h"

/* the top zone of a chip of 2 MB and more, a free gap for the package (ota_esp8266.cpp); false - none (1 MB) */
static inline bool espurna_rbl_top_zone(uint32_t chip, uint32_t* bottom, uint32_t* top) {
    if (chip < 0x200000u) {
        return false;
    }

    *top = chip - ESPURNA_RBL_TOP_TAIL;
    *bottom = *top - ESPURNA_RBL_TOP_ZONE;
    return true;
}

