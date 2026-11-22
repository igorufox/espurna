/*

OTA OF THE HEAD, ESP32: the places of the RBL package (owner 22.11: «делай как ESP8266 у нас общая схема», «у esp32
остается один раздел как у 8266»). The scheme is the ESP8266's (libs/rbl.h, libs/rbl_layout.h, the bootloader step
arch/rtos/bootloader/espurna_rbl.c): the head takes the RBL of a new head while the modules are off, writes the package
right under the header sector and the header last; the bootloader swaps the heads a sector at a time and keeps the
old one compressed until the new one confirms itself (60 s with Wi-Fi, ota_esp32.cpp); ESPURNA_RBL_STARTS starts
without that and the old head comes back.

Flash (code/esp32_4mb.csv, modules_esp32.cpp): the one app partition app0 0x10000..0x3B0000, the head at its start,
the modules' directory in its last sector 0x3AF000; the header sector 0x3AE000 under it (kept from the modules,
Area::keep), the package under the header. The new head must end under the package, the package must start above the
running head (it is written while that one runs)

*/

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../libs/rbl.h"

#define ESPURNA_RBL_SECTOR      0x1000u
#define ESPURNA_RBL_HEAD_AT     0x10000u
#define ESPURNA_RBL_APP_END     0x3B0000u   /* the end of app0 (code/esp32_4mb.csv) */
#define ESPURNA_RBL_HEADER_AT   (ESPURNA_RBL_APP_END - (2u * ESPURNA_RBL_SECTOR))
#define ESPURNA_RBL_NAME        "esp32"
#define ESPURNA_RBL_SECTORS     2u          /* compression: a raw deflate chunk per 4 KB sector */
#define ESPURNA_RBL_FLASH       3u          /* an image of the flash from 0x0: the migration both ways (esp32_migrate.py) */

/* the swap state, the package place, the header check: the same on every chip */
#include "../../libs/rbl_layout.h"
