/*

OTA OF THE HEAD, ESP8266 on ESP8266_RTOS_SDK: the RBL package (owner 2026-11-18: «по схеме tuya в загрузчике»)

One file for the two sides of it, C (the SDK's bootloader, bootloader/espurna_rbl.c) and C++ (the head, ota_esp8266.cpp):

- the head takes the RBL of a new head (espurna-esp8266.ota, scripts/esp8266_module_link.py) while the modules are off,
  writes the package right under the header sector and the 96 bytes of the header last
- the bootloader finds the header at its fixed place, checks the package before it erases anything, inflates it onto
  'factory' (0x10000), checks the result and only then erases the header. A power cut anywhere before that: the
  package is still whole, the next boot installs it again from the start

The header is the RBL of the Beken bootloaders (RT-Thread OTA, ltchiptool soc/bk72xx/util/rbl.py), 96 bytes:
"RBL\0", encryption 0, compression 1 = raw deflate with a 4 KB window (zlib wbits -12: the bootloader keeps the window
in one sector of RAM), the build time, the name "esp8266", the version, sn (unused), the CRC32 of the package, the
FNV-1a of the inflated head, the size of the inflated head, the size of the package, the CRC32 of the 92 bytes before

Flash (arch/esp8266/partitions.csv, modules_esp8266.cpp): the head at 0x10000 grows up, the modules' directory is in
0xF8000 / 0xF9000; the header sector 0xF7000 is under it, the package under the header. The new head must end under the
package (no overlapping inflate), the package must start above the running head (it is written while that one runs)

*/

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ESPURNA_RBL_SECTOR      0x1000u
#define ESPURNA_RBL_HEADER_AT   0xF7000u
#define ESPURNA_RBL_HEAD_AT     0x10000u
#define ESPURNA_RBL_MAGIC       0x004C4252u     /* "RBL\0" */
#define ESPURNA_RBL_NAME        "esp8266"
#define ESPURNA_RBL_DEFLATE     1u
#define ESPURNA_RBL_WINDOW      4096u
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
    uint32_t data_hash;     /* FNV-1a of the inflated head */
    uint32_t raw_size;      /* the inflated head */
    uint32_t data_size;     /* the package */
    uint32_t info_crc;      /* CRC32 of the 92 bytes before */
} espurna_rbl_t;

typedef char espurna_rbl_size_check[(sizeof(espurna_rbl_t) == ESPURNA_RBL_SIZE) ? 1 : -1];

static inline uint32_t espurna_rbl_round(uint32_t value) {
    return (value + ESPURNA_RBL_SECTOR - 1u) & ~(ESPURNA_RBL_SECTOR - 1u);
}

/* where the package lies, named by the head in the header's 'sn' (zero from the build, scripts/esp8266_rbl.py):
   "PKG@" and the flash offset. Chips of 2 MB and more: at the top of the flash under its last 64 KB, out of the
   modules' area (regress-todo.md item 22 stage 2, owner 19.11: the end of the flash, the data modules under it);
   without the tag: right under the header sector, as before (1 MB, and an older head) */
#define ESPURNA_RBL_AT_TAG      "PKG@"
#define ESPURNA_RBL_TOP_TAIL    0x10000u        /* the last 64 KB of the chip: the SDK's system parameters */
#define ESPURNA_RBL_TOP_ZONE    0x80000u        /* 512 KB under it for the package, the data modules below */

static inline uint32_t espurna_rbl_crc32(uint32_t crc, const void* data, size_t size);

static inline uint32_t espurna_rbl_package_at(const espurna_rbl_t* rbl) {
    if (memcmp(rbl->sn, ESPURNA_RBL_AT_TAG, 4) == 0) {
        uint32_t at;
        memcpy(&at, rbl->sn + 4, sizeof(at));
        return at;
    }

    return ESPURNA_RBL_HEADER_AT - espurna_rbl_round(rbl->data_size);
}

/* the top of a chip of 2 MB and more, 0 - none (a chip of 1 MB) */
static inline uint32_t espurna_rbl_top_at(uint32_t chip, uint32_t data_size) {
    if (chip < 0x200000u) {
        return 0;
    }

    const uint32_t size = espurna_rbl_round(data_size);
    if (size > ESPURNA_RBL_TOP_ZONE) {
        return 0;
    }

    return chip - ESPURNA_RBL_TOP_TAIL - size;
}

/* the head names where it wrote the package; the header's CRC follows */
static inline void espurna_rbl_set_package_at(espurna_rbl_t* rbl, uint32_t at) {
    memcpy(rbl->sn, ESPURNA_RBL_AT_TAG, 4);
    memcpy(rbl->sn + 4, &at, sizeof(at));
    rbl->info_crc = espurna_rbl_crc32(0, rbl, ESPURNA_RBL_SIZE - 4u);
}

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

#define ESPURNA_RBL_FNV_START   0x811C9DC5u

static inline uint32_t espurna_rbl_fnv(uint32_t hash, const uint8_t* data, size_t size) {
    while (size--) {
        hash ^= *data++;
        hash *= 0x01000193u;
    }

    return hash;
}

/* the header itself: its CRC, a package this chip takes. NULL - fine, else what is wrong */
static inline const char* espurna_rbl_check(const espurna_rbl_t* rbl) {
    if (rbl->magic != ESPURNA_RBL_MAGIC) {
        return "not an RBL";
    }

    if (espurna_rbl_crc32(0, rbl, ESPURNA_RBL_SIZE - 4u) != rbl->info_crc) {
        return "RBL header CRC";
    }

    if ((rbl->encryption != 0) || (rbl->compression != ESPURNA_RBL_DEFLATE)) {
        return "RBL of another kind (encrypted, or not deflate)";
    }

    if (strncmp(rbl->name, ESPURNA_RBL_NAME, sizeof(rbl->name)) != 0) {
        return "RBL of another chip";
    }

    if (!rbl->data_size || !rbl->raw_size) {
        return "empty RBL";
    }

    if ((rbl->data_size > (ESPURNA_RBL_HEADER_AT - ESPURNA_RBL_HEAD_AT))
        || (rbl->raw_size > (ESPURNA_RBL_HEADER_AT - ESPURNA_RBL_HEAD_AT)))
    {
        return "RBL larger than the flash";
    }

    if ((ESPURNA_RBL_HEAD_AT + rbl->raw_size) > espurna_rbl_package_at(rbl)) {
        return "the new head and its package do not fit the flash together";
    }

    return NULL;
}
