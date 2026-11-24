/*

The place of a new head as an RBL package (libs/rbl.h) for the bootloader step of every chip (arch/rtos/bootloader/
espurna_rbl.c) and the head (ota_<chip>.cpp): the state of a swap in the header sector, where the package lies, what a
package of this chip is. C and C++. The chip's header defines its places first and includes this one:

  ESPURNA_RBL_SECTOR, ESPURNA_RBL_HEADER_AT (the header sector, under the modules' directory), ESPURNA_RBL_HEAD_AT
  (the head: the start of the one app partition), ESPURNA_RBL_NAME (the name of the chip in the header),
  ESPURNA_RBL_SECTORS (the kind: a raw deflate chunk per sector), ESPURNA_RBL_FLASH (an image of the flash)

arch/esp8266/rbl_esp8266.h, arch/esp32/rbl_esp32.h

*/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "rbl.h"

/* the state of a swap in the header sector, after the header (the head writes only the 96 bytes of the header there; the
   rest is 0xFF until the bootloader writes it; bits are cleared one at a time, never set again until the sector is
   erased - by the head's confirmation or at the end of a rollback) */
#define ESPURNA_RBL_SWAP_AT     (ESPURNA_RBL_HEADER_AT + ESPURNA_RBL_SWAP_OFFSET)    /* espurna_rbl_swap_t */
#define ESPURNA_RBL_SAVED_AT    (ESPURNA_RBL_HEADER_AT + 0x200u)    /* a bit per sector: the old one is in the zone */
#define ESPURNA_RBL_WRITTEN_AT  (ESPURNA_RBL_HEADER_AT + 0x240u)    /* a bit per sector: the new one is in place */
#define ESPURNA_RBL_DONE_AT     (ESPURNA_RBL_HEADER_AT + ESPURNA_RBL_DONE_OFFSET)    /* 0: the swap is done */
#define ESPURNA_RBL_BOOTS_AT    (ESPURNA_RBL_HEADER_AT + 0x284u)    /* a bit per start of the new head not confirmed */
#define ESPURNA_RBL_BACK_AT     (ESPURNA_RBL_HEADER_AT + 0x300u)    /* a bit per sector: the old one is back */
#define ESPURNA_RBL_ROLLBACK_AT (ESPURNA_RBL_HEADER_AT + 0x340u)    /* 0: a rollback runs */
/* u16 per sector, the padded size of its chunk: the read part of the package is overwritten by the backup and the
   backup's own sectors may be written again after a power cut - where a chunk starts is never read from them */
#define ESPURNA_RBL_PACKAGE_SIZES_AT (ESPURNA_RBL_HEADER_AT + 0x400u)
#define ESPURNA_RBL_BACKUP_SIZES_AT  (ESPURNA_RBL_HEADER_AT + 0x800u)
/* the sectors of a head the state above holds: 1 KB of u16 per table, 64 bytes of bits per sector table (22.11: the
   tables were 512 bytes, 256 sectors - enough for an ESP8266 head, the ESP32's 322 sectors ran into the next table) */
#define ESPURNA_RBL_SECTORS_MAX 512u
#define ESPURNA_RBL_STARTS      3u                                  /* starts of a new head without its confirmation */


static inline uint32_t espurna_rbl_round(uint32_t value) {
    return (value + ESPURNA_RBL_SECTOR - 1u) & ~(ESPURNA_RBL_SECTOR - 1u);
}

/* a chunk of the package: u16 length (little endian), the raw deflate data of one sector, padded to 4 bytes. What the
   chunk with this first word takes in the flash, the length word too */
static inline uint32_t espurna_rbl_chunk_span(uint32_t first_word) {
    return (2u + (first_word & 0xFFFFu) + 3u) & ~3u;
}

/* a chunk that holds its sector as one stored deflate block (scripts/esp8266_rbl.py STORED_BELOW: the bootloader
   sectors of an image of the flash): length, BFINAL|BTYPE 00, LEN, NLEN, then the sector. The sector's bytes or NULL */
#define ESPURNA_RBL_STORED_CHUNK (2u + 5u + ESPURNA_RBL_SECTOR)
static inline const uint8_t* espurna_rbl_stored_sector(const uint8_t* chunk) {
    const uint32_t length = (uint32_t)chunk[0] | ((uint32_t)chunk[1] << 8);
    const uint32_t len = (uint32_t)chunk[3] | ((uint32_t)chunk[4] << 8);
    const uint32_t nlen = (uint32_t)chunk[5] | ((uint32_t)chunk[6] << 8);
    if ((length != (ESPURNA_RBL_STORED_CHUNK - 2u)) || (chunk[2] != 0x01u) || (len != ESPURNA_RBL_SECTOR)
        || ((len ^ 0xFFFFu) != nlen))
    {
        return NULL;
    }

    return &chunk[7];
}

/* where the package lies (owner 24.11, one rule on every chip): the head puts it at the top of the highest free gap
   of the flash it fits (ota_rbl.cpp rbl::place(); the gaps are the chip's: under the header or under the data of the
   modules, the ESP8266's top zone), and names it in the header's 'sn' (zero from the build, scripts/esp8266_rbl.py):
   "PKG@", the package's offset, the bottom of its gap. Without the tag: right under the header sector, the gap from the
   heads up. The bootloader keeps the old head inside that gap (zone_bottom()): nothing outside of it is written */

static inline uint32_t espurna_rbl_package_at(const espurna_rbl_t* rbl) {
    if (memcmp(rbl->sn, ESPURNA_RBL_AT_TAG, 4) == 0) {
        uint32_t at;
        memcpy(&at, rbl->sn + 4, sizeof(at));
        return at;
    }

    return ESPURNA_RBL_HEADER_AT - espurna_rbl_round(rbl->data_size);
}

/* where the inflated image goes: the head's place, or 0x0 for an image of the flash */
static inline uint32_t espurna_rbl_dest(const espurna_rbl_t* rbl) {
    return (rbl->compression == ESPURNA_RBL_FLASH) ? 0u : ESPURNA_RBL_HEAD_AT;
}

/* the header itself (libs/rbl.h), and a package this chip takes. NULL - fine, else what is wrong */
static inline const char* espurna_rbl_check(const espurna_rbl_t* rbl) {
    const char* error = espurna_rbl_header_check(rbl);
    if (error) {
        return error;
    }

    const bool kind = (rbl->compression == ESPURNA_RBL_SECTORS) || (rbl->compression == ESPURNA_RBL_FLASH);
    if ((rbl->encryption != 0) || !kind) {
        return "RBL of another kind (encrypted, or not deflate chunks)";
    }

    if (strncmp(rbl->name, ESPURNA_RBL_NAME, sizeof(rbl->name)) != 0) {
        return "RBL of another chip";
    }

    if (!rbl->data_size || !rbl->raw_size) {
        return "empty RBL";
    }

    if (((rbl->raw_size + ESPURNA_RBL_SECTOR - 1u) / ESPURNA_RBL_SECTOR) > ESPURNA_RBL_SECTORS_MAX) {
        return "RBL of a head larger than the swap state holds";
    }

    /* the image ends under the header sector: the header, the modules' directory and the settings above it stay */
    const uint32_t dest = espurna_rbl_dest(rbl);
    if ((rbl->data_size > (ESPURNA_RBL_HEADER_AT - ESPURNA_RBL_HEAD_AT))
        || (rbl->raw_size > (ESPURNA_RBL_HEADER_AT - dest)))
    {
        return "RBL larger than the flash";
    }

    if ((dest + rbl->raw_size) > espurna_rbl_package_at(rbl)) {
        return "the new image and its package do not fit the flash together";
    }

    return NULL;
}
