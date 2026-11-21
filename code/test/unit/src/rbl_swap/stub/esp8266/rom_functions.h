/* host test stub (rbl_swap): the ROM's flash chip record */
#pragma once
#include <stdint.h>
typedef struct {
    uint32_t chip_size;
} rom_flashchip_t;
extern rom_flashchip_t g_rom_flashchip;
