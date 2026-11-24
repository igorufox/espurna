/*

FAL of FlashDB for the ESP32 (Matter's key-value store on the 'kvs' partition, matter_port.cpp): one flash device, the
32 KiB of the data partition "spiffs" right below the settings pool (its top 8 sectors, storage_flash_esp32.cpp),
found at its init (fal_flash_esp32.c); the 'kvs' partition is all of it, as on the Beken layouts

*/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define FAL_PRINTF(...) ((void)0)
#define FAL_DEBUG 0

extern const struct fal_flash_dev flash0;

#ifdef __cplusplus
} // extern "C"
#endif

#define FAL_FLASH_DEV_NAME "flash0"
#define FAL_FLASH_DEV_TABLE \
    {                       \
        &flash0,            \
    }

#define FAL_DEV_NAME_MAX 16

// the size of 'kvs', as on the Beken layouts
#define FLASH_KVS_LENGTH 0x8000

#define FAL_PART_HAS_TABLE_CFG
#define FAL_PART_TABLE                                   \
    {                                                    \
        {                                                \
            .magic_word = FAL_PART_MAGIC_WORD,           \
            .name = "kvs",                               \
            .flash_name = FAL_FLASH_DEV_NAME,            \
            .offset = 0,                                 \
            .len = FLASH_KVS_LENGTH,                     \
        },                                               \
    }

#include <fal_def.h>
