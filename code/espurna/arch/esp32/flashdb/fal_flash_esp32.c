/*

The flash device of FAL (FlashDB, fal_cfg.h of this folder, scripts/flashdb.py) on the ESP32: the 32 KiB of the data
partition "spiffs" right below the settings pool (its top 8 sectors, storage_flash_esp32.cpp). Offsets of the device
are offsets in that window. Reads come from a map of the window: esp_partition_read() stops the other core for every
read, and FlashDB reads its records 32 bytes at a time (the loop task starved, the task watchdog fired at the first
Matter start 2026-11-23); writes and erases drop the cached lines of what they changed

*/

#include <fal.h>

#include <esp_partition.h>

#include <string.h>

// esp_private/cache_utils.h (as modules_esp32.cpp): drops the cached lines of the mapped pages of a flash range
extern bool spi_flash_check_and_flush_cache(size_t start_addr, size_t length);

#define FLASH_ERASE_MIN_SIZE (4 * 1024)
// the settings pool at the top of "spiffs" (storage_flash_esp32.cpp Sectors)
#define POOL_SIZE (8 * FLASH_ERASE_MIN_SIZE)

static const esp_partition_t* partition = NULL;
static size_t base = 0;
static const uint8_t* map = NULL;
static esp_partition_mmap_handle_t map_handle;

static int init(void) {
    partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (!partition || (partition->size < (POOL_SIZE + FLASH_KVS_LENGTH))) {
        partition = NULL;
        return -1;
    }

    base = partition->size - POOL_SIZE - FLASH_KVS_LENGTH;
    const void* out = NULL;
    if (esp_partition_mmap(partition, base, FLASH_KVS_LENGTH, ESP_PARTITION_MMAP_DATA, &out, &map_handle) != ESP_OK) {
        partition = NULL;
        return -1;
    }

    map = (const uint8_t*) out;
    return 0;
}

static int read(long offset, uint8_t* buf, size_t size) {
    if (!map || ((offset + size) > FLASH_KVS_LENGTH)) {
        return -1;
    }
    memcpy(buf, map + offset, size);
    return size;
}

static int write(long offset, const uint8_t* buf, size_t size) {
    if (!partition || (esp_partition_write(partition, base + offset, buf, size) != ESP_OK)) {
        return -1;
    }
    spi_flash_check_and_flush_cache(partition->address + base + offset, size);
    return size;
}

static int erase(long offset, size_t size) {
    if (!partition) {
        return -1;
    }

    offset &= ~(FLASH_ERASE_MIN_SIZE - 1);
    size = (((size - 1) / FLASH_ERASE_MIN_SIZE) + 1) * FLASH_ERASE_MIN_SIZE;
    if (esp_partition_erase_range(partition, base + offset, size) != ESP_OK) {
        return -1;
    }
    spi_flash_check_and_flush_cache(partition->address + base + offset, size);
    return size;
}

const struct fal_flash_dev flash0 = {
    .name = FAL_FLASH_DEV_NAME,
    .addr = 0x0,
    .len = FLASH_KVS_LENGTH,
    .blk_size = FLASH_ERASE_MIN_SIZE,
    .ops = {init, read, write, erase},
    .write_gran = 1,
};
