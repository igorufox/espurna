/*

The ESPurna step of the bootloader, ONE for every chip that takes its head as an RBL package (ESP8266, ESP32; owner
22.11 «делай как ESP8266 у нас общая схема», one app partition on both): bootloader_after_init() of the bootloader's
bootloader_hooks.h (the SDK's / ESP-IDF's bootloader_start.c calls it right after bootloader_init(), this one replaces
the weak one): a new head left by the head as an RBL package (libs/rbl.h, the places in arch/<chip>/rbl_<chip>.h) goes
onto the app partition. What the chip's bootloader and ROM force (the flash size for the ROM, the clock, the watchdog,
the reset, the RAM of the buffers) is arch/<chip>/bootloader/rbl_chip.h. Owner 2026-11-18: the Tuya bootloader's scheme; owner 20.11: the heads swap
places («поменять местами»), the old one kept compressed for a rollback, one bootloader on every chip size.

1. no valid header at ESPURNA_RBL_HEADER_AT - nothing to do, the head boots as always
2. the CRC32 of the whole package is checked BEFORE anything is erased: a bad package is left alone, the old head boots
3. a dry run: the running head (its length by esp_image_load) deflated sector by sector, the sizes counted against the
   zone - the free bottom of the package's zone, which grows as the package is read. It fits: the swap record goes to
   the header sector and the heads swap a sector at a time - the old sector deflated into the zone, the new one inflated
   in its place, a bit for each step in the header sector (a power cut resumes there). It does not fit (or the running
   head is no valid image): the new head is installed without a rollback, as before
4. the FNV-1a of the new head is read back; the first start of it is counted. The head confirms itself after 60 s with
   Wi-Fi by erasing the header sector (ota_esp8266.cpp); ESPURNA_RBL_STARTS starts without that - the old head goes
   back (its chunks inflated and checked against its FNV-1a BEFORE anything is erased; a broken backup - the new head
   stays)

5. an image of the flash (compression ESPURNA_RBL_FLASH, owner 21.11: ESPurna 1.15 back over the air): the CRC and a
   dry inflate against the FNV-1a first, then the image from 0x0, from the top sector down - this bootloader last (a
   power cut before that starts it over). No swap, no rollback; the header erased, the chip reset as eboot does it.
   Kept sectors (a chunk of length 0) stay as they are: the ESP32 migration (scripts/esp32_migrate.py) keeps the NVS

The package and the backup are the same chunks (u16 length, raw deflate of one 4 KB sector, padding to 4 bytes): one
inflate for both. The bootloader deflates with the fixed Huffman codes of RFC 1951 (LZ77 inside the sector, hash chains),
a stored block when that is not smaller.

Linked into the bootloader only, never into the head: ESP8266 scripts/esp8266_rtos_sdk.py (before the bootloader's
archives of the SDK package), ESP32 the bootloader component code/bootloader_components/espurna_rbl (ESP-IDF takes the
project's bootloader_components into its bootloader).

*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_image_format.h"

/* bootloader_support/src/bootloader_flash.c (its header is private to that component), the same on both bootloaders */
esp_err_t bootloader_flash_read(size_t src_addr, void* dest, size_t size, bool allow_decrypt);
esp_err_t bootloader_flash_write(size_t dest_addr, void* src, size_t size, bool write_encrypted);
esp_err_t bootloader_flash_erase_sector(size_t sector);

/* the hook of bootloader_hooks.h (weak in the bootloader, private to its main component on ESP-IDF): this one is it */
void bootloader_after_init(void);

/* the chip: its layout (ESPURNA_RBL_* and espurna_rbl_check() of arch/<chip>/rbl_<chip>.h), RBL_PRINTF, and what its
   bootloader forces (RBL_CHIP_BEGIN / END, RBL_FAST / SLOW, RBL_FEED, RBL_RESET, RBL_RAM, RBL_CHIP_FLASH_IMAGE) */
#include "rbl_chip.h"

#ifndef RBL_CHIP_BEGIN
#define RBL_CHIP_BEGIN()
#endif
#ifndef RBL_CHIP_END
#define RBL_CHIP_END()
#endif
#ifndef RBL_FAST
#define RBL_FAST()
#endif
#ifndef RBL_SLOW
#define RBL_SLOW()
#endif
/* a watchdog the bootloader runs: fed between the sectors */
#ifndef RBL_FEED
#define RBL_FEED()
#endif
#ifndef RBL_CHIP_FLASH_IMAGE
#define RBL_CHIP_FLASH_IMAGE 0
#endif
/* the image check of the bootloader, without loading it (ESP8266 SDK: esp_image_load; ESP-IDF 5: esp_image_verify) */
#ifndef RBL_IMAGE_CHECK
#define RBL_IMAGE_CHECK(part, data) esp_image_load(ESP_IMAGE_VERIFY_SILENT, (part), (data))
#endif

#define SECTOR ESPURNA_RBL_SECTOR

/* the host test (test/unit/src/rbl_swap) prints where a step fails; nothing in the bootloader */
#ifndef RBL_TRACE
#define RBL_TRACE(...)
#endif
#define INPUT_SIZE 4096u
#define HASH_BITS 10u

/* the buffers (~28 KB): static where the bootloader has the RAM (ESP8266), else RBL_RAM - RAM the bootloader does not use
   while the step runs (ESP32: the app's DRAM, loaded only after it) */
typedef struct {
    uint8_t in[INPUT_SIZE] __attribute__((aligned(4)));
    uint8_t out[ESPURNA_RBL_SECTOR] __attribute__((aligned(4)));
    uint8_t old[ESPURNA_RBL_SECTOR] __attribute__((aligned(4)));
    uint8_t cmp[ESPURNA_RBL_SECTOR + 16u] __attribute__((aligned(4)));   /* u16 length, the deflate, padding */
    uint16_t hash_head[1u << HASH_BITS];
    uint16_t hash_prev[ESPURNA_RBL_SECTOR];
} rbl_ram_t;

#ifdef RBL_RAM
#define S_RAM ((rbl_ram_t*)(RBL_RAM))
#else
static rbl_ram_t s_ram;
#define S_RAM (&s_ram)
#endif

#define s_in (S_RAM->in)
#define s_out (S_RAM->out)
#define s_old (S_RAM->old)
#define s_cmp (S_RAM->cmp)
#define s_hash_head (S_RAM->hash_head)
#define s_hash_prev (S_RAM->hash_prev)

static uint32_t round_up(uint32_t value) {
    return (value + SECTOR - 1u) & ~(SECTOR - 1u);
}

/* --- the flash: words and bits of the header sector ------------------------------------------------------------ */

static uint32_t word_get(uint32_t at) {
    uint32_t value = 0;
    bootloader_flash_read(at, &value, sizeof(value), false);
    return value;
}

static void word_put(uint32_t at, uint32_t value) {
    bootloader_flash_write(at, &value, sizeof(value), false);
}

static bool bit_done(uint32_t base, uint32_t index) {
    return !(word_get(base + (4u * (index / 32u))) & (1u << (index % 32u)));
}

static void bit_mark(uint32_t base, uint32_t index) {
    const uint32_t at = base + (4u * (index / 32u));
    word_put(at, word_get(at) & ~(1u << (index % 32u)));
}

/* the u16 tables of chunk sizes: two per word, the other half written as 0xFFFF (a write only clears bits) */
static uint32_t size_get(uint32_t base, uint32_t index) {
    const uint32_t word = word_get(base + (4u * (index / 2u)));
    return (index & 1u) ? (word >> 16) : (word & 0xFFFFu);
}

static void size_put(uint32_t base, uint32_t index, uint32_t size) {
    word_put(base + (4u * (index / 2u)), (index & 1u) ? ((size << 16) | 0xFFFFu) : (0xFFFF0000u | size));
}

/* --- the input: a chunk in the flash ---------------------------------------------------------------------------- */

static uint32_t s_in_at;            /* the next flash offset to read */
static uint32_t s_in_left;          /* chunk bytes not read yet */
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

/* the chunk at 'at' (4 byte aligned): its deflate size, where the next one starts */
static uint32_t chunk_size(uint32_t at) {
    return word_get(at) & 0xFFFFu;
}

static uint32_t chunk_next(uint32_t at) {
    return at + espurna_rbl_chunk_span(chunk_size(at));
}

/* --- the output: one sector in RAM ----------------------------------------------------------------------------- */

static uint32_t s_out_len;
static uint32_t s_out_max;
static bool s_out_error;

static void out_byte(uint8_t value) {
    if (s_out_len >= s_out_max) {
        s_out_error = true;
        return;
    }

    s_out[s_out_len++] = value;
}

/* a back reference inside the sector (each chunk is a deflate of its own sector) */
static void out_copy(uint32_t distance, uint32_t length) {
    if (!distance || (distance > s_out_len)) {
        s_out_error = true;
        return;
    }

    while (length-- && !s_out_error) {
        out_byte(s_out[s_out_len - distance]);
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

/* the chunk at 'at' into s_out: true when it gives exactly 'size' bytes */
static bool inflate_chunk(uint32_t at, uint32_t size) {
    RBL_FEED();
    s_in_at = at;
    s_in_left = 2u + chunk_size(at);
    s_in_len = 0;
    s_in_pos = 0;
    s_in_error = false;
    s_bits = 0;
    s_bit_count = 0;
    in_byte();
    in_byte();

    s_out_len = 0;
    s_out_max = size;
    s_out_error = false;

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

    return s_out_len == size;
}

/* --- deflate: fixed Huffman codes, LZ77 inside the sector ------------------------------------------------------ */

#define CHAIN 16u
#define MATCH_MAX 258u


static uint32_t s_cmp_pos;
static uint32_t s_cmp_acc;
static uint32_t s_cmp_count;
static bool s_cmp_over;

static void put(uint32_t value, uint32_t count) {
    s_cmp_acc |= value << s_cmp_count;
    s_cmp_count += count;
    while (s_cmp_count >= 8) {
        if (s_cmp_pos < sizeof(s_cmp)) {
            s_cmp[s_cmp_pos++] = (uint8_t)s_cmp_acc;
        } else {
            s_cmp_over = true;
        }
        s_cmp_acc >>= 8;
        s_cmp_count -= 8;
    }
}

/* a Huffman code goes MSB first */
static void put_code(uint32_t code, uint32_t length) {
    uint32_t reversed = 0;
    for (uint32_t index = 0; index < length; ++index) {
        reversed = (reversed << 1) | ((code >> index) & 1u);
    }
    put(reversed, length);
}

static void put_literal(uint32_t value) {
    if (value < 144) {
        put_code(0x30u + value, 8);
    } else {
        put_code(0x190u + (value - 144u), 9);
    }
}

static void put_match(uint32_t length, uint32_t distance) {
    uint32_t index = 28;
    while (s_len_base[index] > length) {
        --index;
    }

    const uint32_t symbol = 257u + index;
    if (symbol < 280) {
        put_code(symbol - 256u, 7);
    } else {
        put_code(0xC0u + (symbol - 280u), 8);
    }
    put(length - s_len_base[index], s_len_extra[index]);

    uint32_t dist = 29;
    while (s_dist_base[dist] > distance) {
        --dist;
    }
    put_code(dist, 5);
    put(distance - s_dist_base[dist], s_dist_extra[dist]);
}

static uint32_t hash3(const uint8_t* data) {
    return ((((uint32_t)data[0] << 10) ^ ((uint32_t)data[1] << 5) ^ data[2]) * 2654435761u) >> (32u - HASH_BITS);
}

/* 'data' of 'size' bytes into s_cmp as a chunk; its padded size */
static uint32_t deflate_chunk(const uint8_t* data, uint32_t size) {
    RBL_FEED();
    s_cmp_pos = 2;
    s_cmp_acc = 0;
    s_cmp_count = 0;
    s_cmp_over = false;

    memset(s_hash_head, 0xFF, sizeof(s_hash_head));

    put(1, 1);      /* the last block */
    put(1, 2);      /* fixed Huffman codes */

    uint32_t pos = 0;
    while ((pos < size) && !s_cmp_over) {
        uint32_t best = 0;
        uint32_t best_dist = 0;

        if ((pos + 3u) <= size) {
            const uint32_t hash = hash3(&data[pos]);
            uint32_t candidate = s_hash_head[hash];
            uint32_t chain = CHAIN;
            const uint32_t limit = ((size - pos) < MATCH_MAX) ? (size - pos) : MATCH_MAX;

            while ((candidate != 0xFFFFu) && chain--) {
                uint32_t length = 0;
                while ((length < limit) && (data[candidate + length] == data[pos + length])) {
                    ++length;
                }
                if (length > best) {
                    best = length;
                    best_dist = pos - candidate;
                    if (length == limit) {
                        break;
                    }
                }
                candidate = s_hash_prev[candidate];
            }

            s_hash_prev[pos] = (uint16_t)s_hash_head[hash];
            s_hash_head[hash] = (uint16_t)pos;
        }

        if (best >= 3) {
            put_match(best, best_dist);
            for (uint32_t step = 1; step < best; ++step) {
                const uint32_t at = pos + step;
                if ((at + 3u) <= size) {
                    const uint32_t hash = hash3(&data[at]);
                    s_hash_prev[at] = (uint16_t)s_hash_head[hash];
                    s_hash_head[hash] = (uint16_t)at;
                }
            }
            pos += best;
        } else {
            put_literal(data[pos]);
            ++pos;
        }
    }

    put_code(0, 7);     /* the end of the block */
    if (s_cmp_count) {
        put(0, 8u - s_cmp_count);
    }

    uint32_t length = s_cmp_pos - 2u;
    if (s_cmp_over || (length > (size + 5u))) {
        /* a stored block: the header byte (last, type 0), the length and its complement, the bytes */
        s_cmp[2] = 0x01;
        s_cmp[3] = (uint8_t)size;
        s_cmp[4] = (uint8_t)(size >> 8);
        s_cmp[5] = (uint8_t)~size;
        s_cmp[6] = (uint8_t)(~size >> 8);
        memcpy(&s_cmp[7], data, size);
        length = size + 5u;
    }

    s_cmp[0] = (uint8_t)length;
    s_cmp[1] = (uint8_t)(length >> 8);

    const uint32_t padded = (2u + length + 3u) & ~3u;
    memset(&s_cmp[2u + length], 0xFF, padded - (2u + length));
    return padded;
}

/* --- the heads ----------------------------------------------------------------------------------------------- */


/* sector 'index' of the area at 'base' (the head's place, or 0x0 for an image of the flash) into s_old */
static bool area_sector_read(uint32_t base, uint32_t index, uint32_t size) {
    RBL_FEED();
    return bootloader_flash_read(base + (index * SECTOR), s_old, (size + 3u) & ~3u, false) == ESP_OK;
}

static bool head_sector_read(uint32_t index, uint32_t size) {
    return area_sector_read(ESPURNA_RBL_HEAD_AT, index, size);
}

/* s_out (its first 'size' bytes, the rest 0xFF) onto sector 'index' of the area at 'base' */
static bool area_sector_write(uint32_t base, uint32_t index, uint32_t size) {
    if (size < SECTOR) {
        memset(&s_out[size], 0xFF, SECTOR - size);
    }

    const uint32_t at = base + (index * SECTOR);
    return (bootloader_flash_erase_sector(at / SECTOR) == ESP_OK)
        && (bootloader_flash_write(at, s_out, SECTOR, false) == ESP_OK);
}

static bool head_sector_write(uint32_t index, uint32_t size) {
    return area_sector_write(ESPURNA_RBL_HEAD_AT, index, size);
}

static uint32_t sector_size(uint32_t total, uint32_t index) {
    const uint32_t left = total - (index * SECTOR);
    return (left < SECTOR) ? left : SECTOR;
}

static uint32_t sectors_of(uint32_t size) {
    return (size + SECTOR - 1u) / SECTOR;
}

/* the FNV-1a of what the area at 'base' holds now */
static uint32_t area_hash(uint32_t base, uint32_t size) {
    uint32_t hash = ESPURNA_RBL_FNV_START;
    for (uint32_t index = 0; index < sectors_of(size); ++index) {
        const uint32_t take = sector_size(size, index);
        if (!area_sector_read(base, index, take)) {
            return 0;
        }
        hash = espurna_rbl_fnv(hash, s_old, take);
    }

    return hash;
}

static uint32_t head_hash(uint32_t size) {
    return area_hash(ESPURNA_RBL_HEAD_AT, size);
}

/* the length of the running head (the bootloader's image check: segments, checksum), 0 - no valid image */
static uint32_t running_head(void) {
    const esp_partition_pos_t part = {
        .offset = ESPURNA_RBL_HEAD_AT,
        .size = ESPURNA_RBL_HEADER_AT - ESPURNA_RBL_HEAD_AT,
    };
    esp_image_metadata_t data;
    memset(&data, 0, sizeof(data));
    if (RBL_IMAGE_CHECK(&part, &data) != ESP_OK) {
        return 0;
    }

    return data.image_len;
}

/* --- the backup zone ------------------------------------------------------------------------------------------- */

static bool package_crc_ok(const espurna_rbl_t* rbl) {
    uint32_t at = espurna_rbl_package_at(rbl);
    uint32_t left = rbl->data_size;
    uint32_t crc = 0;

    while (left) {
        RBL_FEED();
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

/* the bottom of the zone, the same on every chip: above both heads and inside the package's gap (named by the head in
   the header, libs/rbl_layout.h): what lies under the gap (the modules, their data) is never written */
static uint32_t zone_bottom(const espurna_rbl_t* rbl, uint32_t old_size) {
    const uint32_t larger = (old_size > rbl->raw_size) ? old_size : rbl->raw_size;
    const uint32_t heads = round_up(ESPURNA_RBL_HEAD_AT + larger);
    const uint32_t gap = espurna_rbl_gap_bottom(rbl);
    return (gap > heads) ? gap : heads;
}

static uint32_t zone_top(const espurna_rbl_t* rbl) {
    return espurna_rbl_package_at(rbl) + espurna_rbl_round(rbl->data_size);
}

/* bytes of the flash that are all 0xFF */
static bool all_ff(uint32_t at, uint32_t size) {
    while (size) {
        const uint32_t take = (size < INPUT_SIZE) ? size : INPUT_SIZE;
        if (bootloader_flash_read(at, s_in, take, false) != ESP_OK) {
            return false;
        }
        for (uint32_t index = 0; index < take; ++index) {
            if (s_in[index] != 0xFF) {
                return false;
            }
        }
        at += take;
        size -= take;
    }

    return true;
}

/* the chunk of s_cmp at 'at' of the zone: a sector it reaches into is erased first unless it is blank there; the
   sector it starts in keeps the chunks before it (a power cut left half a chunk: that sector is written again) */
static bool backup_write(uint32_t at, uint32_t size) {
    const uint32_t end = at + size;
    for (uint32_t sector = at & ~(SECTOR - 1u); sector < end; sector += SECTOR) {
        const uint32_t from = (at > sector) ? at : sector;
        const uint32_t to = (end < (sector + SECTOR)) ? end : (sector + SECTOR);
        if (all_ff(from, to - from)) {
            continue;
        }

        const uint32_t keep = (at > sector) ? (at - sector) : 0;
        if (keep && (bootloader_flash_read(sector, s_in, keep, false) != ESP_OK)) {
            RBL_TRACE("backup: read 0x%06x %u\n", sector, keep);
            return false;
        }
        if (bootloader_flash_erase_sector(sector / SECTOR) != ESP_OK) {
            RBL_TRACE("backup: erase 0x%06x\n", sector);
            return false;
        }
        if (keep && (bootloader_flash_write(sector, s_in, keep, false) != ESP_OK)) {
            RBL_TRACE("backup: keep write 0x%06x %u\n", sector, keep);
            return false;
        }
    }

    if (bootloader_flash_write(at, s_cmp, size, false) != ESP_OK) {
        RBL_TRACE("backup: write 0x%06x %u\n", at, size);
        return false;
    }

    return true;
}

/* the dry run: the old head deflated a sector at a time, its chunks must stay under the package that is not read yet
   (a sector of the zone is erased only below the sector the unread package starts in) and inside the zone. Its FNV-1a
   on the way */
static bool backup_fits(const espurna_rbl_t* rbl, uint32_t old_size, uint32_t* old_hash) {
    const uint32_t old_sectors = sectors_of(old_size);
    const uint32_t new_sectors = sectors_of(rbl->raw_size);
    const uint32_t count = (old_sectors > new_sectors) ? old_sectors : new_sectors;

    /* the bit tables and the size tables of the swap hold ESPURNA_RBL_SECTORS_MAX sectors (the new head is checked
     * against it with the header): an old head larger than that is not kept, the new one goes on without a rollback */
    if (old_sectors > ESPURNA_RBL_SECTORS_MAX) {
        return false;
    }

    uint32_t out = zone_bottom(rbl, old_size);
    uint32_t package = espurna_rbl_package_at(rbl);
    uint32_t hash = ESPURNA_RBL_FNV_START;

    for (uint32_t index = 0; index < count; ++index) {
        if (index < old_sectors) {
            const uint32_t take = sector_size(old_size, index);
            if (!head_sector_read(index, take)) {
                return false;
            }
            hash = espurna_rbl_fnv(hash, s_old, take);
            out += deflate_chunk(s_old, take);
            if (out > (package & ~(SECTOR - 1u))) {
                return false;
            }
        }
        if (index < new_sectors) {
            package = chunk_next(package);
        }
    }

    *old_hash = hash;
    return out <= zone_top(rbl);
}

/* --- install, swap, rollback ----------------------------------------------------------------------------------- */

/* without a rollback: the new head sector by sector */
static bool install_plain(const espurna_rbl_t* rbl) {
    uint32_t package = espurna_rbl_package_at(rbl);
    for (uint32_t index = 0; index < sectors_of(rbl->raw_size); ++index) {
        const uint32_t size = sector_size(rbl->raw_size, index);
        if (!inflate_chunk(package, size) || !head_sector_write(index, size)) {
            return false;
        }
        package = chunk_next(package);
    }

    return true;
}

/* the swap, from where the bits say it stopped */
static bool swap(const espurna_rbl_t* rbl, const espurna_rbl_swap_t* record) {
    const uint32_t old_sectors = sectors_of(record->old_size);
    const uint32_t new_sectors = sectors_of(rbl->raw_size);

    uint32_t out = record->backup_at;
    uint32_t package = espurna_rbl_package_at(rbl);

    for (uint32_t index = 0; index < record->sectors; ++index) {
        if (index < old_sectors) {
            if (!bit_done(ESPURNA_RBL_SAVED_AT, index)) {
                const uint32_t take = sector_size(record->old_size, index);
                if (!head_sector_read(index, take)) {
                    RBL_TRACE("swap %u: old sector read\n", index);
                    return false;
                }
                const uint32_t size = deflate_chunk(s_old, take);
                if (!backup_write(out, size)) {
                    RBL_TRACE("swap %u: backup write at 0x%06x size %u\n", index, out, size);
                    return false;
                }
                size_put(ESPURNA_RBL_BACKUP_SIZES_AT, index, size);
                bit_mark(ESPURNA_RBL_SAVED_AT, index);
            }
            out += size_get(ESPURNA_RBL_BACKUP_SIZES_AT, index);
        }

        if (!bit_done(ESPURNA_RBL_WRITTEN_AT, index)) {
            if (index < new_sectors) {
                const uint32_t size = sector_size(rbl->raw_size, index);
                if (!inflate_chunk(package, size) || !head_sector_write(index, size)) {
                    RBL_TRACE("swap %u: new sector from the package at 0x%06x\n", index, package);
                    return false;
                }
            }
            bit_mark(ESPURNA_RBL_WRITTEN_AT, index);
        }

        if (index < new_sectors) {
            package += size_get(ESPURNA_RBL_PACKAGE_SIZES_AT, index);
        }
    }

    return true;
}

/* the backup inflates into the old head: checked whole before anything is written */
static bool backup_ok(const espurna_rbl_swap_t* record) {
    uint32_t at = record->backup_at;
    uint32_t hash = ESPURNA_RBL_FNV_START;
    for (uint32_t index = 0; index < sectors_of(record->old_size); ++index) {
        const uint32_t size = sector_size(record->old_size, index);
        if (!inflate_chunk(at, size)) {
            return false;
        }
        hash = espurna_rbl_fnv(hash, s_out, size);
        at += size_get(ESPURNA_RBL_BACKUP_SIZES_AT, index);
    }

    return hash == record->old_hash;
}

static bool rollback(const espurna_rbl_swap_t* record) {
    uint32_t at = record->backup_at;
    for (uint32_t index = 0; index < sectors_of(record->old_size); ++index) {
        if (!bit_done(ESPURNA_RBL_BACK_AT, index)) {
            const uint32_t size = sector_size(record->old_size, index);
            if (!inflate_chunk(at, size) || !head_sector_write(index, size)) {
                return false;
            }
            bit_mark(ESPURNA_RBL_BACK_AT, index);
        }
        at += size_get(ESPURNA_RBL_BACKUP_SIZES_AT, index);
    }

    return head_hash(record->old_size) == record->old_hash;
}

static bool swap_record_ok(const espurna_rbl_swap_t* record) {
    return (record->magic == ESPURNA_RBL_SWAP_MAGIC)
        && (espurna_rbl_crc32(0, record, sizeof(*record) - 4u) == record->crc);
}

static void header_erase(void) {
    bootloader_flash_erase_sector(ESPURNA_RBL_HEADER_AT / SECTOR);
}

/* the starts of a new head not confirmed yet: bits cleared from the lowest one */
static uint32_t starts(void) {
    const uint32_t word = word_get(ESPURNA_RBL_BOOTS_AT);
    uint32_t count = 0;
    while ((count < 32u) && !(word & (1u << count))) {
        ++count;
    }
    return count;
}

static void start_count(void) {
    const uint32_t count = starts();
    if (count < 32u) {
        bit_mark(ESPURNA_RBL_BOOTS_AT, count);
    }
}

/* --- an image of the flash from 0x0 (ESPURNA_RBL_FLASH: another firmware with its own bootloader) -------------- */

#if RBL_CHIP_FLASH_IMAGE


/* where the chunk of sector 'index' starts: counted from the package's start (the package lies above the image and is
   never written here, so this holds after a power cut too) */
static uint32_t chunk_of(const espurna_rbl_t* rbl, uint32_t index) {
    uint32_t at = espurna_rbl_package_at(rbl);
    while (index--) {
        at = chunk_next(at);
    }
    return at;
}

/* a chunk of length 0: a kept sector (owner 22.11, the ESP32 migration both ways): the flash there stays as it is (the
   NVS of ESP-IDF), it is not in the FNV-1a of the image either (scripts/esp8266_rbl.py, keep). A raw deflate is never
   empty, so no other chunk looks like that */
static bool chunk_kept(uint32_t at) {
    return chunk_size(at) == 0u;
}

/* every chunk inflates and the image they give has the FNV-1a of the header: checked before anything is erased */
static bool package_inflates(const espurna_rbl_t* rbl) {
    uint32_t at = espurna_rbl_package_at(rbl);
    uint32_t hash = ESPURNA_RBL_FNV_START;
    for (uint32_t index = 0; index < sectors_of(rbl->raw_size); ++index) {
        const uint32_t size = sector_size(rbl->raw_size, index);
        if (chunk_kept(at)) {
            at = chunk_next(at);
            continue;
        }
        if (!inflate_chunk(at, size)) {
            RBL_TRACE("flash image: chunk %u at 0x%06x does not inflate\n", index, at);
            return false;
        }
        hash = espurna_rbl_fnv(hash, s_out, size);
        at = chunk_next(at);
    }

    return hash == rbl->data_hash;
}

/* from the top sector down: this bootloader (the first sectors) goes last. A power cut before them boots this
   bootloader again, the header is still there and it starts over */
static bool install_flash(const espurna_rbl_t* rbl) {
    for (uint32_t index = sectors_of(rbl->raw_size); index--; ) {
        const uint32_t size = sector_size(rbl->raw_size, index);
        const uint32_t chunk = chunk_of(rbl, index);
        if (chunk_kept(chunk)) {
            continue;
        }
        if (!inflate_chunk(chunk, size) || !area_sector_write(0, index, size)) {
            RBL_TRACE("flash image: sector %u\n", index);
            return false;
        }
    }

    /* read back: the FNV-1a of the written sectors, the kept ones left out as in the package */
    uint32_t at = espurna_rbl_package_at(rbl);
    uint32_t hash = ESPURNA_RBL_FNV_START;
    for (uint32_t index = 0; index < sectors_of(rbl->raw_size); ++index) {
        const uint32_t take = sector_size(rbl->raw_size, index);
        const bool kept = chunk_kept(at);
        at = chunk_next(at);
        if (kept) {
            continue;
        }
        if (!area_sector_read(0, index, take)) {
            return false;
        }
        hash = espurna_rbl_fnv(hash, s_old, take);
    }

    return hash == rbl->data_hash;
}

static void flash_image(const espurna_rbl_t* rbl) {
    RBL_FAST();
    const bool good = package_crc_ok(rbl) && package_inflates(rbl);
    RBL_SLOW();
    if (!good) {
        RBL_PRINTF("ESPURNA RBL: the image of the flash does not check out, the head stays\n");
        return;
    }

    RBL_PRINTF("ESPURNA RBL: writing an image of the flash, %u bytes (package %u at 0x%06x) from 0x0\n",
        rbl->raw_size, rbl->data_size, espurna_rbl_package_at(rbl));

    RBL_FAST();
    bool ok = false;
    for (uint32_t attempt = 0; (attempt < 3u) && !ok; ++attempt) {
        ok = install_flash(rbl);
    }
    RBL_SLOW();

    /* this bootloader is gone from 0x0 now whatever happened: the chip starts over from the new image */
    if (ok) {
        header_erase();
    }
    RBL_PRINTF(ok ? "ESPURNA RBL: the image of the flash is written (read back), reset\n"
        : "ESPURNA RBL: the flash differs from the image, reset\n");
    RBL_RESET();
}

#endif // RBL_CHIP_FLASH_IMAGE


static void rbl_step(void) {
    espurna_rbl_t rbl;
    if (bootloader_flash_read(ESPURNA_RBL_HEADER_AT, &rbl, sizeof(rbl), false) != ESP_OK) {
        return;
    }

    if (rbl.magic != ESPURNA_RBL_MAGIC) {
        return;
    }

    const char* error = espurna_rbl_check(&rbl);
    if (error) {
        RBL_PRINTF("ESPURNA RBL: %s, the head stays\n", error);
        return;
    }

#if RBL_CHIP_FLASH_IMAGE
    if (rbl.compression == ESPURNA_RBL_FLASH) {
        flash_image(&rbl);
        return;
    }
#endif

    espurna_rbl_swap_t record;
    bootloader_flash_read(ESPURNA_RBL_SWAP_AT, &record, sizeof(record), false);
    const bool swapping = swap_record_ok(&record);

    /* a swap done: count the starts of the new head, its confirmation erases all this */
    if (swapping && !word_get(ESPURNA_RBL_DONE_AT)) {
        const bool rolling = !word_get(ESPURNA_RBL_ROLLBACK_AT);
        if (!rolling && (starts() < ESPURNA_RBL_STARTS)) {
            start_count();
            return;
        }

        RBL_PRINTF("ESPURNA RBL: the new head did not confirm itself in %u starts, the old head back\n",
            ESPURNA_RBL_STARTS);
        RBL_FAST();
        /* the backup is checked once, before the first write. A rollback cut half way (the mark is set) goes on from
         * the backup at the next start, whatever the head holds by then */
        const bool backup = rolling || backup_ok(&record);
        if (backup && !rolling) {
            word_put(ESPURNA_RBL_ROLLBACK_AT, 0);
        }
        const bool ok = backup && rollback(&record);
        RBL_SLOW();

        /* the header goes only when nothing is left to do: the old head is back, or the backup was broken before
         * anything of the head was written (the new head stays). A rollback that failed half way keeps the header
         * and starts again: the half written head is never run */
        if (ok || !backup) {
            header_erase();
        }
        RBL_PRINTF(ok ? "ESPURNA RBL: the old head is back (read back)\n"
            : backup ? "ESPURNA RBL: the rollback failed, again after a reset\n"
            : "ESPURNA RBL: the backup is broken, the new head stays\n");
        if (backup && !ok) {
            RBL_RESET();
        }
        return;
    }

    RBL_FAST();
    const bool crc_ok = swapping || package_crc_ok(&rbl);
    RBL_SLOW();

    if (!crc_ok) {
        RBL_PRINTF("ESPURNA RBL: package CRC, the head stays\n");
        return;
    }

    /* a new swap: the running head must fit the zone deflated */
    if (!swapping) {
        RBL_FAST();
        const uint32_t old_size = running_head();
        /* the head in the flash is the package already: an install without a rollback cut by the power after its last
           sector, before the header was erased (test_no_room), or the same head uploaded again - nothing to swap */
        const bool same = (old_size == rbl.raw_size) && (head_hash(old_size) == rbl.data_hash);
        uint32_t old_hash = 0;
        const bool fits = !same && old_size && backup_fits(&rbl, old_size, &old_hash);
        RBL_SLOW();

        if (same) {
            header_erase();
            RBL_PRINTF("ESPURNA RBL: the head is the package already (read back)\n");
            return;
        }

        if (fits) {
            /* the sizes of the package's chunks while it is whole (written again the same after a power cut here) */
            uint32_t package = espurna_rbl_package_at(&rbl);
            for (uint32_t index = 0; index < sectors_of(rbl.raw_size); ++index) {
                const uint32_t next = chunk_next(package);
                size_put(ESPURNA_RBL_PACKAGE_SIZES_AT, index, next - package);
                package = next;
            }

            record.magic = ESPURNA_RBL_SWAP_MAGIC;
            record.old_size = old_size;
            record.old_hash = old_hash;
            record.backup_at = zone_bottom(&rbl, old_size);
            record.sectors = sectors_of((old_size > rbl.raw_size) ? old_size : rbl.raw_size);
            record.crc = espurna_rbl_crc32(0, &record, sizeof(record) - 4u);
            bootloader_flash_write(ESPURNA_RBL_SWAP_AT, &record, sizeof(record), false);
        } else {
            RBL_PRINTF("ESPURNA RBL: installing %u bytes (package %u), no room to keep the old head\n",
                rbl.raw_size, rbl.data_size);
            RBL_FAST();
            const bool ok = install_plain(&rbl) && (head_hash(rbl.raw_size) == rbl.data_hash);
            RBL_SLOW();
            if (!ok) {
                RBL_PRINTF("ESPURNA RBL: install failed, again on the next boot\n");
                return;
            }
            header_erase();
            RBL_PRINTF("ESPURNA RBL: installed (read back)\n");
            return;
        }
    }

    RBL_PRINTF("ESPURNA RBL: installing %u bytes (package %u), the old head %u kept at 0x%06x\n",
        rbl.raw_size, rbl.data_size, record.old_size, record.backup_at);

    RBL_FAST();
    const bool ok = swap(&rbl, &record);
    const bool read_back = ok && (head_hash(rbl.raw_size) == rbl.data_hash);
    RBL_SLOW();

    if (!ok) {
        /* the bits stay: the next boot goes on from the sector that failed */
        RBL_PRINTF("ESPURNA RBL: install failed, again on the next boot\n");
        return;
    }

    word_put(ESPURNA_RBL_DONE_AT, 0);
    if (!read_back) {
        /* the new head is not what the package says: the old one back at once */
        RBL_PRINTF("ESPURNA RBL: the head in the flash differs from the package, the old head back\n");
        word_put(ESPURNA_RBL_BOOTS_AT, 0);
        rbl_step();
        return;
    }

    start_count();
    RBL_PRINTF("ESPURNA RBL: installed (read back)\n");
}

void bootloader_after_init(void) {
    RBL_CHIP_BEGIN();
    rbl_step();
    RBL_CHIP_END();
}
