/*

The ESPurna step of the SDK's bootloader: bootloader_after_init() of the SDK's bootloader_hooks.h (bootloader_start.c
calls it right after bootloader_init(), this one replaces the weak one): a new head left by the head as an RBL package
(../rbl_esp8266.h) goes onto 'factory'. Owner 2026-11-18: the Tuya bootloader's scheme.

1. no valid header at ESPURNA_RBL_HEADER_AT - nothing to do, the head boots as always
2. the CRC32 of the whole package is checked BEFORE anything is erased: a bad package is left alone, the old head boots
3. the package is inflated sector by sector onto 0x10000 (raw deflate, 4 KB window = the one sector buffer in RAM)
4. the FNV-1a and the size of what was written are compared with the header; only then the header sector is erased
   A power cut in 3 or 4: the header and the package are untouched, the next boot does it all again

Linked into the bootloader only (scripts/esp8266_rtos_sdk.py, before the bootloader's archives of the SDK package),
never into the head.

*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "rom/ets_sys.h"

/* bootloader_support/src/bootloader_flash.c (its header is private to that component) */
esp_err_t bootloader_flash_read(size_t src_addr, void* dest, size_t size, bool allow_decrypt);
esp_err_t bootloader_flash_write(size_t dest_addr, void* src, size_t size, bool write_encrypted);
esp_err_t bootloader_flash_erase_sector(size_t sector);
uint32_t bootloader_read_flash_id(void);

#include "esp8266/rom_functions.h"

/* the size of the chip by its JEDEC id (capacity byte 0x14..0x18 = 1..16 MB), as the head does (core_esp8266.cpp
   flash_size_from_id): the ROM takes it from the image header (the SDK's bootloader.bin says 1 MB), and the ROM's
   SPIRead refuses anything over it - 19.11 the package at the top of the 4 MB chip was never read, the head stayed */
static void chip_size_from_id(void) {
    const uint32_t capacity = bootloader_read_flash_id() & 0xffu;
    if ((capacity >= 0x14u) && (capacity <= 0x18u)) {
        g_rom_flashchip.chip_size = 1u << capacity;
    }
}


#include "bootloader_hooks.h"

#include "../rbl_esp8266.h"

/* --- the input: the package in the flash ---------------------------------------------------------------------- */

#define INPUT_SIZE 4096u

/* the ROM's erase of a 64 KB block (esp8266.rom.ld: SPIEraseSector 0x40004a00, SPIEraseBlock 0x400049b4): ~150 ms where
   16 sectors take ~720 ms; where the head is 64 KB aligned and at least that much is left */
#define BLOCK_SIZE 0x10000u
typedef int (*rom_erase_block_t)(uint32_t block);
#define ROM_ERASE_BLOCK ((rom_erase_block_t)0x400049b4u)

/* the CPU at 160 MHz while installing (DPORT_CTL_DOUBLE_CLK, as bootloader_start.c with CONFIG_BOOTLOADER_FAST_BOOT) */
#define DPORT_CTL (*(volatile uint32_t*)0x3FF00014u)

static uint32_t s_erased_to;        /* the flash offset erased up to */

static uint32_t s_in_at;            /* the next flash offset to read */
static uint32_t s_in_left;          /* package bytes not read yet */
static uint8_t s_in[INPUT_SIZE] __attribute__((aligned(4)));
static uint32_t s_in_len;
static uint32_t s_in_pos;
static bool s_in_error;

static uint32_t s_bits;             /* the bit buffer, LSB first */
static uint32_t s_bit_count;

static int in_byte(void) {
    if (s_in_pos == s_in_len) {
        if (!s_in_left) {
            s_in_error = true;
            return 0;
        }

        uint32_t take = (s_in_left < INPUT_SIZE) ? s_in_left : INPUT_SIZE;
        uint32_t aligned = (take + 3u) & ~3u;
        if (bootloader_flash_read(s_in_at, s_in, aligned, false) != ESP_OK) {
            s_in_error = true;
            return 0;
        }

        s_in_at += aligned;
        s_in_left -= take;
        s_in_len = take;
        s_in_pos = 0;
    }

    return s_in[s_in_pos++];
}

static uint32_t bits(uint32_t count) {
    while (s_bit_count < count) {
        s_bits |= (uint32_t)in_byte() << s_bit_count;
        s_bit_count += 8;
    }

    uint32_t out = s_bits & ((1u << count) - 1u);
    s_bits >>= count;
    s_bit_count -= count;
    return out;
}

/* --- the output: the head, a sector at a time ----------------------------------------------------------------- */

static uint8_t s_out[ESPURNA_RBL_WINDOW] __attribute__((aligned(4)));
static uint32_t s_out_len;          /* bytes written in all */
static uint32_t s_out_max;          /* the size of the head (header raw_size) */
static uint32_t s_hash;
static bool s_out_error;

static void out_flush(uint32_t size) {
    const uint32_t sector = (s_out_len - 1u) / ESPURNA_RBL_SECTOR;
    const uint32_t offset = ESPURNA_RBL_HEAD_AT + (sector * ESPURNA_RBL_SECTOR);

    if (size < ESPURNA_RBL_SECTOR) {
        memset(&s_out[size], 0xFF, ESPURNA_RBL_SECTOR - size);
    }

    while (s_erased_to <= offset) {
        const uint32_t left = (ESPURNA_RBL_HEAD_AT + s_out_max) - s_erased_to;
        if (!(s_erased_to & (BLOCK_SIZE - 1u)) && (left >= BLOCK_SIZE)) {
            if (ROM_ERASE_BLOCK(s_erased_to / BLOCK_SIZE) != 0) {
                s_out_error = true;
                return;
            }
            s_erased_to += BLOCK_SIZE;
        } else {
            if (bootloader_flash_erase_sector(s_erased_to / ESPURNA_RBL_SECTOR) != ESP_OK) {
                s_out_error = true;
                return;
            }
            s_erased_to += ESPURNA_RBL_SECTOR;
        }
    }

    if (bootloader_flash_write(offset, s_out, (size + 3u) & ~3u, false) != ESP_OK) {
        s_out_error = true;
    }
}

static void out_byte(uint8_t value) {
    if (s_out_len >= s_out_max) {
        s_out_error = true;
        return;
    }

    s_out[s_out_len & (ESPURNA_RBL_WINDOW - 1u)] = value;
    s_hash = (s_hash ^ value) * 0x01000193u;
    ++s_out_len;

    if (!(s_out_len & (ESPURNA_RBL_WINDOW - 1u))) {
        out_flush(ESPURNA_RBL_WINDOW);
    }
}

/* a back reference: the window is the sector buffer (distance <= 4096, zlib wbits -12) */
static void out_copy(uint32_t distance, uint32_t length) {
    if (!distance || (distance > ESPURNA_RBL_WINDOW) || (distance > s_out_len)) {
        s_out_error = true;
        return;
    }

    while (length-- && !s_out_error) {
        out_byte(s_out[(s_out_len - distance) & (ESPURNA_RBL_WINDOW - 1u)]);
    }
}

/* --- inflate (RFC 1951) ---------------------------------------------------------------------------------------- */

typedef struct {
    uint16_t counts[16];
    uint16_t symbols[288];
} tree_t;

static tree_t s_lit;
static tree_t s_dist;

static const uint16_t s_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};

static const uint8_t s_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};

static const uint16_t s_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577,
};

static const uint8_t s_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};

static void tree_build(tree_t* tree, const uint8_t* lengths, uint32_t count) {
    uint16_t offsets[16];

    memset(tree->counts, 0, sizeof(tree->counts));
    for (uint32_t index = 0; index < count; ++index) {
        tree->counts[lengths[index]]++;
    }
    tree->counts[0] = 0;

    uint16_t sum = 0;
    for (uint32_t index = 0; index < 16; ++index) {
        offsets[index] = sum;
        sum += tree->counts[index];
    }

    for (uint32_t index = 0; index < count; ++index) {
        if (lengths[index]) {
            tree->symbols[offsets[lengths[index]]++] = (uint16_t)index;
        }
    }
}

static int tree_decode(const tree_t* tree) {
    int code = 0;
    int first = 0;
    int index = 0;

    for (uint32_t length = 1; length < 16; ++length) {
        code |= (int)bits(1);
        const int count = tree->counts[length];
        if ((code - first) < count) {
            return tree->symbols[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }

    s_in_error = true;
    return 0;
}

static void trees_fixed(void) {
    uint8_t lengths[288];
    uint32_t index = 0;

    for (; index < 144; ++index) lengths[index] = 8;
    for (; index < 256; ++index) lengths[index] = 9;
    for (; index < 280; ++index) lengths[index] = 7;
    for (; index < 288; ++index) lengths[index] = 8;
    tree_build(&s_lit, lengths, 288);

    for (index = 0; index < 30; ++index) lengths[index] = 5;
    tree_build(&s_dist, lengths, 30);
}

static bool trees_dynamic(void) {
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    uint8_t lengths[288 + 32];

    const uint32_t hlit = bits(5) + 257u;
    const uint32_t hdist = bits(5) + 1u;
    const uint32_t hclen = bits(4) + 4u;
    if ((hlit > 286) || (hdist > 30)) {
        return false;
    }

    memset(lengths, 0, 19);
    for (uint32_t index = 0; index < hclen; ++index) {
        lengths[order[index]] = (uint8_t)bits(3);
    }

    tree_t code_tree;
    tree_build(&code_tree, lengths, 19);

    uint32_t index = 0;
    while (index < (hlit + hdist)) {
        const int symbol = tree_decode(&code_tree);
        if (s_in_error) {
            return false;
        }

        if (symbol < 16) {
            lengths[index++] = (uint8_t)symbol;
            continue;
        }

        uint8_t value = 0;
        uint32_t repeat = 0;
        if (symbol == 16) {
            if (!index) {
                return false;
            }
            value = lengths[index - 1];
            repeat = 3 + bits(2);
        } else if (symbol == 17) {
            repeat = 3 + bits(3);
        } else {
            repeat = 11 + bits(7);
        }

        if ((index + repeat) > (hlit + hdist)) {
            return false;
        }

        while (repeat--) {
            lengths[index++] = value;
        }
    }

    tree_build(&s_lit, lengths, hlit);
    tree_build(&s_dist, lengths + hlit, hdist);
    return true;
}

static bool block_huffman(void) {
    for (;;) {
        const int symbol = tree_decode(&s_lit);
        if (s_in_error || s_out_error) {
            return false;
        }

        if (symbol < 256) {
            out_byte((uint8_t)symbol);
            continue;
        }

        if (symbol == 256) {
            return true;
        }

        const int index = symbol - 257;
        if (index >= 29) {
            return false;
        }

        const uint32_t length = s_len_base[index] + bits(s_len_extra[index]);
        const int dist_symbol = tree_decode(&s_dist);
        if ((dist_symbol < 0) || (dist_symbol >= 30)) {
            return false;
        }

        const uint32_t distance = s_dist_base[dist_symbol] + bits(s_dist_extra[dist_symbol]);
        out_copy(distance, length);
    }
}

static bool block_stored(void) {
    /* to the byte boundary */
    s_bits = 0;
    s_bit_count = 0;

    const uint32_t length = (uint32_t)in_byte() | ((uint32_t)in_byte() << 8);
    const uint32_t inverse = (uint32_t)in_byte() | ((uint32_t)in_byte() << 8);
    if ((length ^ 0xFFFFu) != inverse) {
        return false;
    }

    for (uint32_t index = 0; (index < length) && !s_in_error && !s_out_error; ++index) {
        out_byte((uint8_t)in_byte());
    }

    return true;
}

static bool inflate(void) {
    uint32_t last = 0;
    while (!last) {
        last = bits(1);
        const uint32_t type = bits(2);

        bool ok = false;
        switch (type) {
        case 0:
            ok = block_stored();
            break;
        case 1:
            trees_fixed();
            ok = block_huffman();
            break;
        case 2:
            ok = trees_dynamic() && block_huffman();
            break;
        default:
            break;
        }

        if (!ok || s_in_error || s_out_error) {
            return false;
        }
    }

    /* what is left in the buffer: the last, partial sector */
    const uint32_t tail = s_out_len & (ESPURNA_RBL_WINDOW - 1u);
    if (tail) {
        out_flush(tail);
    }

    return !s_out_error;
}

/* --- the step ------------------------------------------------------------------------------------------------- */

static bool package_crc_ok(const espurna_rbl_t* rbl) {
    uint32_t at = espurna_rbl_package_at(rbl);
    uint32_t left = rbl->data_size;
    uint32_t crc = 0;

    while (left) {
        const uint32_t take = (left < INPUT_SIZE) ? left : INPUT_SIZE;
        if (bootloader_flash_read(at, s_in, (take + 3u) & ~3u, false) != ESP_OK) {
            return false;
        }

        crc = espurna_rbl_crc32(crc, s_in, take);
        at += (take + 3u) & ~3u;
        left -= take;
    }

    return crc == rbl->data_crc;
}

/* the head as it is in the flash now: its FNV-1a read back (the hash of the inflate is over what went to the writes,
   an erase or a write the flash did not take passed it: 18.11 the head after an install reported the size of the head
   before it, regress-todo.md item 23) */
static bool written_ok(const espurna_rbl_t* rbl) {
    uint32_t at = ESPURNA_RBL_HEAD_AT;
    uint32_t left = rbl->raw_size;
    uint32_t hash = ESPURNA_RBL_FNV_START;

    while (left) {
        const uint32_t take = (left < INPUT_SIZE) ? left : INPUT_SIZE;
        if (bootloader_flash_read(at, s_in, (take + 3u) & ~3u, false) != ESP_OK) {
            return false;
        }
        for (uint32_t index = 0; index < take; ++index) {
            hash = (hash ^ s_in[index]) * 0x01000193u;
        }
        at += take;
        left -= take;
    }

    return hash == rbl->data_hash;
}

void bootloader_after_init(void) {
    chip_size_from_id();

    espurna_rbl_t rbl;
    if (bootloader_flash_read(ESPURNA_RBL_HEADER_AT, &rbl, sizeof(rbl), false) != ESP_OK) {
        return;
    }

    if (rbl.magic != ESPURNA_RBL_MAGIC) {
        return;
    }

    const char* error = espurna_rbl_check(&rbl);
    if (error) {
        ets_printf("ESPURNA RBL: %s, the head stays\n", error);
        return;
    }

    /* 160 MHz for the CRC, the inflate and the read back; ets_printf() only at 80 MHz (the UART divider) */
    const uint32_t clock = DPORT_CTL;
    DPORT_CTL = clock | 1u;
    const bool crc_ok = package_crc_ok(&rbl);
    DPORT_CTL = clock;

    if (!crc_ok) {
        ets_printf("ESPURNA RBL: package CRC, the head stays\n");
        return;
    }

    ets_printf("ESPURNA RBL: installing %u bytes (package %u)\n", rbl.raw_size, rbl.data_size);

    s_in_at = espurna_rbl_package_at(&rbl);
    s_in_left = rbl.data_size;
    s_in_len = 0;
    s_in_pos = 0;
    s_in_error = false;
    s_bits = 0;
    s_bit_count = 0;

    s_out_len = 0;
    s_out_max = rbl.raw_size;
    s_hash = ESPURNA_RBL_FNV_START;
    s_out_error = false;
    s_erased_to = ESPURNA_RBL_HEAD_AT;

    DPORT_CTL = clock | 1u;
    const bool ok = inflate();
    const bool read_back = ok && written_ok(&rbl);
    DPORT_CTL = clock;

    if (!ok || (s_out_len != rbl.raw_size) || (s_hash != rbl.data_hash)) {
        /* the header stays: the next boot tries again from the start */
        ets_printf("ESPURNA RBL: install failed (%u of %u bytes), again on the next boot\n", s_out_len, rbl.raw_size);
        return;
    }

    if (!read_back) {
        /* the header stays: the next boot writes it again */
        ets_printf("ESPURNA RBL: the head in the flash differs from the package, again on the next boot\n");
        return;
    }

    bootloader_flash_erase_sector(ESPURNA_RBL_HEADER_AT / ESPURNA_RBL_SECTOR);
    ets_printf("ESPURNA RBL: installed (read back)\n");
}
