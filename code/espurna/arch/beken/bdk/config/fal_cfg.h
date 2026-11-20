/*

FAL of FlashDB for the BK7231N (Matter's key-value store on the 'kvs' partition, arch/beken/matter_beken.cpp): one
flash device (port/fal_flash_beken.c) and the partitions of the Tuya layout (FLASH_*_OFFSET / _LENGTH of
scripts/beken_bdk.py, from the board's flash layout). As LibreTiny's config/fal_cfg.h (MIT, Kuba Szczodrzyński)

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

#define FAL_PART_HAS_TABLE_CFG
#define FAL_PART_TABLE_ITEM(part_lower, part_upper)  \
    {                                                \
        .magic_word = FAL_PART_MAGIC_WORD,           \
        .name = #part_lower,                         \
        .flash_name = FAL_FLASH_DEV_NAME,            \
        .offset = FLASH_##part_upper##_OFFSET,       \
        .len = FLASH_##part_upper##_LENGTH,          \
    },

#define FAL_PART_TABLE                               \
    {                                                \
        FAL_PART_TABLE_ITEM(download, DOWNLOAD)      \
        FAL_PART_TABLE_ITEM(kvs, KVS)                \
    }

#include <fal_def.h>
