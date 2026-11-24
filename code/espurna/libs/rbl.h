/*

The RBL header of the OTA of every chip that takes one (ESP8266: arch/esp8266/rbl_esp8266.h and its bootloader; Beken:
arch/beken/ota_beken.cpp), C and C++. The RBL of RT-Thread OTA (ltchiptool soc/bk72xx/util/rbl.py), 96 bytes: "RBL\0",
encryption, compression, the build time, the name, the version, sn, the CRC32 of the package, the FNV-1a of the inflated
image, the size of the inflated image, the size of the package, the CRC32 of the 92 bytes before. The package follows
it. What the package is (compression, name) and where it goes is the chip's; the head writes it with ota_rbl.h

*/

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ESPURNA_RBL_MAGIC       0x004C4252u     /* "RBL\0" */
#define ESPURNA_RBL_SIZE        96u

typedef struct {
    uint32_t magic;
    uint8_t encryption;
    uint8_t compression;
    uint8_t reserved[2];
    uint32_t timestamp;
    char name[16];
    char version[24];
    char sn[24];
    uint32_t data_crc;      /* CRC32 of the package */
    uint32_t data_hash;     /* FNV-1a of the inflated image */
    uint32_t raw_size;      /* the inflated image */
    uint32_t data_size;     /* the package */
    uint32_t info_crc;      /* CRC32 of the 92 bytes before */
} espurna_rbl_t;

typedef char espurna_rbl_size_check[(sizeof(espurna_rbl_t) == ESPURNA_RBL_SIZE) ? 1 : -1];

/* the place of the package in the header's 'sn' (libs/rbl_layout.h): "PKG@", the offset, the bottom of its free gap */
#define ESPURNA_RBL_AT_TAG      "PKG@"

/* CRC32 (zlib's), a nibble table: small code for the bootloader */
static inline uint32_t espurna_rbl_crc32(uint32_t crc, const void* data, size_t size) {
    static const uint32_t table[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };

    const uint8_t* ptr = (const uint8_t*)data;
    crc = ~crc;
    while (size--) {
        crc ^= *ptr++;
        crc = (crc >> 4) ^ table[crc & 0x0Fu];
        crc = (crc >> 4) ^ table[crc & 0x0Fu];
    }

    return ~crc;
}

/* the bottom of the package's free gap, 0 - from the heads up */
static inline uint32_t espurna_rbl_gap_bottom(const espurna_rbl_t* rbl) {
    uint32_t out = 0;
    if (memcmp(rbl->sn, ESPURNA_RBL_AT_TAG, 4) == 0) {
        memcpy(&out, rbl->sn + 8, sizeof(out));
    }

    return out;
}

/* the head names where it wrote the package and the bottom of its gap; the header's CRC follows */
static inline void espurna_rbl_set_place(espurna_rbl_t* rbl, uint32_t at, uint32_t gap_bottom) {
    memcpy(rbl->sn, ESPURNA_RBL_AT_TAG, 4);
    memcpy(rbl->sn + 4, &at, sizeof(at));
    memcpy(rbl->sn + 8, &gap_bottom, sizeof(gap_bottom));
    rbl->info_crc = espurna_rbl_crc32(0, rbl, ESPURNA_RBL_SIZE - 4u);
}

#define ESPURNA_RBL_FNV_START   0x811C9DC5u

static inline uint32_t espurna_rbl_fnv(uint32_t hash, const uint8_t* data, size_t size) {
    while (size--) {
        hash ^= *data++;
        hash *= 0x01000193u;
    }

    return hash;
}


/* the state of a swap the bootloader keeps in the header sector, after the header (libs/rbl_layout.h): the record of the
   old head, and the word that says the swap is done - what the head's confirmation looks for */
#define ESPURNA_RBL_SWAP_OFFSET 0x100u
#define ESPURNA_RBL_DONE_OFFSET 0x280u
#define ESPURNA_RBL_SWAP_MAGIC  0x50415753u     /* "SWAP" */

typedef struct {
    uint32_t magic;
    uint32_t old_size;      /* the running head that is backed up */
    uint32_t old_hash;      /* its FNV-1a */
    uint32_t backup_at;     /* its chunks, from the bottom of the zone up */
    uint32_t sectors;       /* the sectors swapped: the larger head of the two */
    uint32_t crc;           /* CRC32 of the 20 bytes before */
} espurna_rbl_swap_t;

/* the header of every RBL: the magic, its own CRC, a package. NULL - fine, else what is wrong */
static inline const char* espurna_rbl_header_check(const espurna_rbl_t* rbl) {
    if (rbl->magic != ESPURNA_RBL_MAGIC) {
        return "not an RBL";
    }

    if (espurna_rbl_crc32(0, rbl, ESPURNA_RBL_SIZE - 4u) != rbl->info_crc) {
        return "RBL header CRC";
    }

    if (!rbl->data_size) {
        return "empty RBL";
    }

    return NULL;
}
