/*

The flash device of FAL (FlashDB, config/fal_cfg.h) on the BDK's flash driver, as LibreTiny's
port/fal_flash_bk72xx_port.c (MIT, Kuba Szczodrzyński)

*/

#include "../bdk.h"

#include <fal.h>

#define FLASH_ERASE_MIN_SIZE (4 * 1024)

static void unprotect(void) {
    if (get_flash_protect() != FLASH_PROTECT_NONE) {
        flash_protection_op(0, FLASH_PROTECT_NONE);
    }
}

static int init(void) {
    flash_init();
    flash_ctrl(CMD_FLASH_WRITE_ENABLE, NULL);
    unprotect();
    return 0;
}

static int read(long offset, uint8_t* buf, size_t size) {
    flash_read((char*)buf, size, offset);
    return size;
}

static int write(long offset, const uint8_t* buf, size_t size) {
    unprotect();
    flash_write((char*)buf, size, offset);
    return size;
}

static int erase(long offset, size_t size) {
    unprotect();
    offset &= ~(FLASH_ERASE_MIN_SIZE - 1);
    size = ((size - 1) / FLASH_ERASE_MIN_SIZE) + 1;
    for (size_t i = 0; i < size; i++) {
        uint32_t addr = offset + i * FLASH_ERASE_MIN_SIZE;
        flash_ctrl(CMD_FLASH_ERASE_SECTOR, &addr);
    }

    return size * FLASH_ERASE_MIN_SIZE;
}

const struct fal_flash_dev flash0 = {
    .name = FAL_FLASH_DEV_NAME,
    .addr = 0x0,
    .len = FLASH_LENGTH,
    .blk_size = FLASH_ERASE_MIN_SIZE,
    .ops = {init, read, write, erase},
    .write_gran = 1,
};
