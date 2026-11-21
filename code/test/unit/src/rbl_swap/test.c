/*
 * Host test of the ESP8266 bootloader step (espurna/arch/esp8266/bootloader/espurna_rbl.c, owner 20.11: the heads swap
 * places, the old one kept compressed for a rollback, one bootloader on every chip size): the real C file on a flash of
 * 1 MB and 4 MB in RAM (NOR: a write only clears bits, an erase sets a sector to 0xFF).
 *
 *   swap       the new head in place, the old one in the zone, nothing outside the zone touched
 *   confirm    the head erases the header sector: the next boots leave everything alone
 *   rollback   ESPURNA_RBL_STARTS starts without that: the old head back, the header erased
 *   power cut  the first boot stops after N flash operations (a write half done, an erase half done), the next boot
 *              must finish the swap; the same for a rollback
 *   no room    an old head that does not fit the zone deflated: the new head installed without a rollback
 *
 * Built and run by run.sh (WSL): sh run.sh <old head.bin> <new head.bin> <new head.rbl>
 */

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DPORT_CTL s_dport
static uint32_t s_dport;

#include "esp_err.h"
#include "esp_image_format.h"

/* the chip's reset after an image of the flash is written: back into boot() */
#define RBL_RESET() longjmp(s_power, 2)
static jmp_buf s_power;
static bool s_reset;

#define FLASH_MAX 0x400000u
static uint8_t s_flash[FLASH_MAX];
static uint32_t s_chip;
static long s_cut = -1;         /* flash operations left before the power cut, -1: none */
static long s_ops;
static const char* s_what = "";
static size_t s_where;
static const char* s_cut_what = "";
static size_t s_cut_where;

#include "esp8266/rom_functions.h"
rom_flashchip_t g_rom_flashchip;

static void power(void) {
    ++s_ops;
    if (s_cut >= 0) {
        if (s_cut == 0) {
            s_cut_what = s_what;
            s_cut_where = s_where;
            longjmp(s_power, 1);
        }
        --s_cut;
    }
}

esp_err_t bootloader_flash_read(size_t at, void* out, size_t size, bool decrypt) {
    (void)decrypt;
    if ((at & 3u) || ((at + size) > s_chip)) {
        return ESP_FAIL;
    }
    memcpy(out, &s_flash[at], size);
    return ESP_OK;
}

esp_err_t bootloader_flash_write(size_t at, void* in, size_t size, bool encrypt) {
    (void)encrypt;
    if ((at & 3u) || (size & 3u) || ((at + size) > s_chip)) {
        return ESP_FAIL;
    }
    const uint8_t* data = (const uint8_t*)in;
    s_what = "write"; s_where = at;
    if (s_cut == 0) {
        /* half of it got there */
        for (size_t index = 0; index < (size / 2); ++index) {
            s_flash[at + index] &= data[index];
        }
    }
    power();
    for (size_t index = 0; index < size; ++index) {
        s_flash[at + index] &= data[index];
    }
    return ESP_OK;
}

esp_err_t bootloader_flash_erase_sector(size_t sector) {
    const size_t at = sector * 4096u;
    if ((at + 4096u) > s_chip) {
        return ESP_FAIL;
    }
    s_what = "erase"; s_where = at;
    if (s_cut == 0) {
        memset(&s_flash[at], 0xFF, 2048);
    }
    power();
    memset(&s_flash[at], 0xFF, 4096);
    return ESP_OK;
}

uint32_t bootloader_read_flash_id(void) {
    uint32_t capacity = 0;
    while ((1u << capacity) < s_chip) {
        ++capacity;
    }
    return capacity;
}

/* the image check of the SDK: the heads of this test are valid images when they are whole */
static uint8_t* g_old;
static uint32_t g_old_len;
static uint8_t* g_new;
static uint32_t g_new_len;

esp_err_t esp_image_load(esp_image_load_mode_t mode, const esp_partition_pos_t* part, esp_image_metadata_t* data) {
    (void)mode;
    if (!memcmp(&s_flash[part->offset], g_old, g_old_len)) {
        data->image_len = g_old_len;
        return ESP_OK;
    }
    if (!memcmp(&s_flash[part->offset], g_new, g_new_len)) {
        data->image_len = g_new_len;
        return ESP_OK;
    }
    return ESP_FAIL;
}

#include "espurna_rbl.c"

static uint8_t* load(const char* path, uint32_t* size) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "no %s\n", path);
        exit(2);
    }
    fseek(file, 0, SEEK_END);
    *size = (uint32_t)ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t* out = malloc(*size);
    if (fread(out, 1, *size, file) != *size) {
        exit(2);
    }
    fclose(file);
    return out;
}

static uint8_t* s_rbl;
static uint32_t s_rbl_len;
static uint8_t s_initial[FLASH_MAX];
static int s_failures;

#define CHECK(cond, ...) do { if (!(cond)) { ++s_failures; printf("FAIL %s:%d ", __func__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* the flash as the head leaves it after an OTA: the old head, the package and its header, the rest a pattern */
static void flash_setup(uint32_t chip) {
    s_chip = chip;
    g_rom_flashchip.chip_size = 0x100000u;
    for (uint32_t index = 0; index < chip; ++index) {
        s_flash[index] = (uint8_t)(0xA5u ^ (index >> 12));
    }
    memset(&s_flash[ESPURNA_RBL_HEAD_AT], 0xFF, ESPURNA_RBL_HEADER_AT - ESPURNA_RBL_HEAD_AT);
    memcpy(&s_flash[ESPURNA_RBL_HEAD_AT], g_old, g_old_len);

    espurna_rbl_t header;
    memcpy(&header, s_rbl, sizeof(header));
    const uint32_t data_size = s_rbl_len - ESPURNA_RBL_SIZE;
    const uint32_t top = espurna_rbl_top_at(chip, data_size);
    if (top) {
        espurna_rbl_set_package_at(&header, top);
    }
    const uint32_t package = espurna_rbl_package_at(&header);
    memset(&s_flash[package], 0xFF, espurna_rbl_round(data_size));
    memcpy(&s_flash[package], s_rbl + ESPURNA_RBL_SIZE, data_size);

    memset(&s_flash[ESPURNA_RBL_HEADER_AT], 0xFF, ESPURNA_RBL_SECTOR);
    memcpy(&s_flash[ESPURNA_RBL_HEADER_AT], &header, sizeof(header));
    memcpy(s_initial, s_flash, chip);
}

/* one start of the board; false - the power went away */
static bool boot(long cut) {
    s_cut = cut;
    s_ops = 0;
    s_reset = false;
    const int jumped = setjmp(s_power);
    if (jumped == 1) {
        s_cut = -1;
        return false;
    }
    if (jumped == 2) {
        s_cut = -1;
        s_reset = true;
        return true;
    }
    bootloader_after_init();
    s_cut = -1;
    return true;
}

static bool head_is(const uint8_t* head, uint32_t size) {
    return !memcmp(&s_flash[ESPURNA_RBL_HEAD_AT], head, size);
}

static bool header_gone(void) {
    return word_get(ESPURNA_RBL_HEADER_AT) == 0xFFFFFFFFu;
}

/* the flash outside the head area, the zone and the header sector is what it was */
static bool outside_untouched(void) {
    espurna_rbl_t header;
    memcpy(&header, &s_initial[ESPURNA_RBL_HEADER_AT], sizeof(header));
    const uint32_t lo = zone_bottom(&header, g_old_len);
    const uint32_t hi = zone_top(&header);
    for (uint32_t at = 0; at < s_chip; ++at) {
        const bool head = (at >= ESPURNA_RBL_HEAD_AT) && (at < lo) && (at < ESPURNA_RBL_HEADER_AT);
        const bool zone = (at >= lo) && (at < hi);
        const bool sector = (at >= ESPURNA_RBL_HEADER_AT) && (at < (ESPURNA_RBL_HEADER_AT + ESPURNA_RBL_SECTOR));
        if (!head && !zone && !sector && (s_flash[at] != s_initial[at])) {
            printf("  changed at 0x%06x\n", at);
            return false;
        }
    }
    return true;
}

static void test_swap(uint32_t chip) {
    flash_setup(chip);
    CHECK(boot(-1), "boot");
    printf("  chip %u KB: swap %ld flash operations\n", chip / 1024u, s_ops);
    CHECK(head_is(g_new, g_new_len), "the new head in place");
    CHECK(!word_get(ESPURNA_RBL_DONE_AT), "the swap done");
    CHECK(starts() == 1, "one start counted (%u)", starts());
    CHECK(outside_untouched(), "nothing outside the zone");

    espurna_rbl_swap_t record;
    bootloader_flash_read(ESPURNA_RBL_SWAP_AT, &record, sizeof(record), false);
    CHECK(swap_record_ok(&record) && backup_ok(&record), "the backup inflates into the old head");

    /* confirmed: nothing happens any more */
    uint8_t copy[ESPURNA_RBL_SECTOR];
    memset(copy, 0xFF, sizeof(copy));
    bootloader_flash_erase_sector(ESPURNA_RBL_HEADER_AT / ESPURNA_RBL_SECTOR);
    for (int index = 0; index < 5; ++index) {
        CHECK(boot(-1), "boot");
        CHECK(!s_ops, "confirmed: no flash operation (%ld)", s_ops);
    }
    CHECK(head_is(g_new, g_new_len), "the new head stays");
}

static void test_rollback(uint32_t chip) {
    flash_setup(chip);
    CHECK(boot(-1), "boot");
    for (uint32_t index = 1; index < ESPURNA_RBL_STARTS; ++index) {
        CHECK(boot(-1), "boot");
        CHECK(head_is(g_new, g_new_len), "start %u: still the new head", index + 1u);
    }
    CHECK(boot(-1), "boot");
    printf("  chip %u KB: rollback %ld flash operations\n", chip / 1024u, s_ops);
    CHECK(head_is(g_old, g_old_len), "the old head back");
    CHECK(header_gone(), "the header erased");
    CHECK(outside_untouched(), "nothing outside the zone");
}

static void test_power_cut(uint32_t chip, long step) {
    flash_setup(chip);
    CHECK(boot(-1), "boot");
    const long total = s_ops;
    int cuts = 0;
    for (long cut = 0; cut < total; cut += step) {
        memcpy(s_flash, s_initial, chip);
        if (boot(cut)) {
            continue;
        }
        ++cuts;
        /* the next start, and one more in case that one was the start the swap counted */
        boot(-1);
        if (!head_is(g_new, g_new_len) || word_get(ESPURNA_RBL_DONE_AT)) {
            CHECK(false, "cut after %ld of %ld operations (%s 0x%06zx): the swap not finished", cut, total, s_cut_what, s_cut_where);
            return;
        }
        espurna_rbl_swap_t record;
        bootloader_flash_read(ESPURNA_RBL_SWAP_AT, &record, sizeof(record), false);
        if (!backup_ok(&record)) {
            /* a cut inside the rewrite of a zone sector may cost the backup: then no rollback, never a broken head */
            printf("  cut after %ld: the backup lost (allowed, the rollback refuses it)\n", cut);
        }
        CHECK(outside_untouched(), "cut after %ld: nothing outside the zone", cut);
    }
    printf("  chip %u KB: %d power cuts in a swap of %ld operations\n", chip / 1024u, cuts, total);
}

static void test_rollback_power_cut(uint32_t chip, long step) {
    flash_setup(chip);
    for (uint32_t index = 0; index < ESPURNA_RBL_STARTS; ++index) {
        boot(-1);
    }
    uint8_t* swapped = malloc(chip);
    memcpy(swapped, s_flash, chip);
    CHECK(boot(-1), "boot");
    const long total = s_ops;
    int cuts = 0;
    for (long cut = 0; cut < total; cut += step) {
        memcpy(s_flash, swapped, chip);
        if (boot(cut)) {
            continue;
        }
        ++cuts;
        boot(-1);
        CHECK(head_is(g_old, g_old_len) || head_is(g_new, g_new_len),
            "rollback cut after %ld of %ld: a whole head", cut, total);
        if (!head_is(g_old, g_old_len)) {
            printf("  rollback cut after %ld: the new head stays\n", cut);
        }
    }
    printf("  chip %u KB: %d power cuts in a rollback of %ld operations\n", chip / 1024u, cuts, total);
    free(swapped);
}

/* an old head that does not deflate (random bytes, 520 KB): no room for it in the zone - the new head without a rollback;
   a power cut in that install leaves an old head that is no image any more, the next boot installs it again */
static void test_no_room(uint32_t chip) {
    uint8_t* keep = g_old;
    const uint32_t keep_len = g_old_len;
    g_old_len = 520u * 1024u;
    g_old = malloc(g_old_len);
    unsigned long long state = 0x2545F4914F6CDD1Dull;
    for (uint32_t index = 0; index < g_old_len; ++index) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        g_old[index] = (uint8_t)state;
    }

    flash_setup(chip);
    CHECK(boot(-1), "boot");
    const long total = s_ops;
    CHECK(head_is(g_new, g_new_len), "no room: the new head in place");
    CHECK(header_gone(), "no room: the header erased (no rollback)");

    int cuts = 0;
    for (long cut = 0; cut < total; cut += 5) {
        memcpy(s_flash, s_initial, chip);
        if (boot(cut)) {
            continue;
        }
        ++cuts;
        boot(-1);
        CHECK(head_is(g_new, g_new_len) && header_gone(), "no room, cut after %ld of %ld: installed", cut, total);
    }
    printf("  chip %u KB: no room - installed without a rollback, %d power cuts of %ld operations\n",
        chip / 1024u, cuts, total);

    free(g_old);
    g_old = keep;
    g_old_len = keep_len;
}

/* an image of the flash from 0x0 (ESPURNA_RBL_FLASH, another firmware): the bootloader of this test is the first 5
   sectors (0x0..0x5000, as the SDK's 16.8 KB one with its padding segment). A power cut while one of them is rewritten
   costs the wire - counted, never hidden; a cut anywhere else must be finished by the next boot */
#define BOOTLOADER_SECTORS 5u

static uint8_t* s_flash_rbl;
static uint32_t s_flash_rbl_len;

static void flash_image_setup(uint32_t chip) {
    uint8_t* keep = s_rbl;
    const uint32_t keep_len = s_rbl_len;
    s_rbl = s_flash_rbl;
    s_rbl_len = s_flash_rbl_len;
    flash_setup(chip);
    s_rbl = keep;
    s_rbl_len = keep_len;
}

/* the image in place from 0x0, the rest of the flash as it was but the header sector (erased) */
static bool flash_image_done(void) {
    espurna_rbl_t header;
    memcpy(&header, s_flash_rbl, sizeof(header));
    if (memcmp(s_flash, g_new, g_new_len)) {
        return false;
    }
    for (uint32_t at = g_new_len; at < s_chip; ++at) {
        const bool tail = at < round_up(g_new_len);
        const bool sector = (at >= ESPURNA_RBL_HEADER_AT) && (at < (ESPURNA_RBL_HEADER_AT + ESPURNA_RBL_SECTOR));
        if (tail ? (s_flash[at] != 0xFF) : (!sector && (s_flash[at] != s_initial[at]))) {
            printf("  flash image: changed at 0x%06x\n", at);
            return false;
        }
    }
    return header_gone();
}

static bool bootloader_whole(void) {
    return !memcmp(s_flash, s_initial, BOOTLOADER_SECTORS * ESPURNA_RBL_SECTOR);
}

static void test_flash_image(uint32_t chip, long step) {
    flash_image_setup(chip);
    CHECK(boot(-1) && s_reset, "flash image: the chip reset at the end");
    const long total = s_ops;
    CHECK(flash_image_done(), "flash image: the image from 0x0, nothing else touched");

    /* a broken package: nothing erased, the head stays */
    flash_image_setup(chip);
    espurna_rbl_t header;
    memcpy(&header, &s_flash[ESPURNA_RBL_HEADER_AT], sizeof(header));
    s_flash[espurna_rbl_package_at(&header) + 100u] ^= 0x10u;
    s_initial[espurna_rbl_package_at(&header) + 100u] ^= 0x10u;
    CHECK(boot(-1) && !s_reset && !s_ops, "flash image, broken package: no flash operation (%ld)", s_ops);

    int cuts = 0;
    int bricked = 0;
    for (long cut = 0; cut < total; cut += step) {
        flash_image_setup(chip);
        if (boot(cut)) {
            continue;
        }
        ++cuts;
        if (!bootloader_whole()) {
            ++bricked;
            continue;
        }
        if (!boot(-1) || !s_reset || !flash_image_done()) {
            CHECK(false, "flash image cut after %ld of %ld (%s 0x%06zx): not finished", cut, total, s_cut_what, s_cut_where);
            return;
        }
    }
    printf("  chip %u KB: flash image %ld operations, %d power cuts, %d of them inside the bootloader's %u sectors\n",
        chip / 1024u, total, cuts, bricked, BOOTLOADER_SECTORS);
    CHECK((bricked * step) <= ((long)BOOTLOADER_SECTORS * 2 + step), "the bootloader's window: %d cuts", bricked);
}

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "test <old head.bin> <new head.bin> <new head.rbl> <new head as an image of the flash.rbl>\n");
        return 2;
    }

    g_old = load(argv[1], &g_old_len);
    g_new = load(argv[2], &g_new_len);
    s_rbl = load(argv[3], &s_rbl_len);
    s_flash_rbl = load(argv[4], &s_flash_rbl_len);

    /* the deflate of the bootloader against its own inflate, every sector of both heads */
    for (uint32_t index = 0; index < sectors_of(g_old_len); ++index) {
        const uint32_t size = sector_size(g_old_len, index);
        const uint32_t chunk = deflate_chunk(&g_old[index * 4096u], size);
        s_chip = FLASH_MAX;
        memcpy(&s_flash[0x200000], s_cmp, chunk);
        if (!inflate_chunk(0x200000, size) || memcmp(s_out, &g_old[index * 4096u], size)) {
            CHECK(false, "deflate/inflate of sector %u", index);
            break;
        }
    }

    const long step = (argc > 5) ? atol(argv[5]) : 7;
    const uint32_t chips[] = { 0x100000u, 0x400000u };
    for (unsigned index = 0; index < 2; ++index) {
        test_swap(chips[index]);
        test_rollback(chips[index]);
        test_power_cut(chips[index], step);
        test_rollback_power_cut(chips[index], step);
        test_no_room(chips[index]);
        test_flash_image(chips[index], step);
    }

    printf("%s: %d failures\n", s_failures ? "FAIL" : "PASS", s_failures);
    return s_failures ? 1 : 0;
}
