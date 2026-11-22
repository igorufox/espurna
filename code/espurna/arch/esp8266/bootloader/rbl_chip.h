/*

The ESP8266 part of the bootloader step (arch/rtos/bootloader/espurna_rbl.c): what the SDK's bootloader and this chip's
ROM force, nothing of the install itself. Included once, by espurna_rbl.c (scripts/esp8266_rtos_sdk.py, -I of this
folder)

- the layout: ../rbl_esp8266.h (the header sector, the head, the package's place and zones, the chip's header check)
- the size of the flash chip by its JEDEC id for the ROM while the step runs (RBL_CHIP_BEGIN / RBL_CHIP_END)
- the CPU at 160 MHz while writing (RBL_FAST / RBL_SLOW), the UART prints at 80 MHz only
- the reset after an image of the flash (RBL_RESET), and that image itself (RBL_CHIP_FLASH_IMAGE: ESPurna 1.15 back)
- the XMC flash startup of the SDK (bootloader_flash_xmc_startup, --wrap)

*/

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* the host test (test/unit/src/rbl_swap) replaces the ROM, the clock register and the reset */
#include "rom/ets_sys.h"
#include "esp8266/rom_functions.h"

#include "../rbl_esp8266.h"

#define RBL_PRINTF ets_printf

uint32_t bootloader_read_flash_id(void);
esp_err_t bootloader_flash_xmc_startup(void);

/* the size of the chip by its JEDEC id (capacity byte 0x14..0x18 = 1..16 MB), as the head does (core_esp8266.cpp
   flash_size_from_id): the ROM takes it from the image header (the SDK's bootloader.bin says 1 MB), and the ROM's
   SPIRead refuses anything over it - 19.11 the package at the top of the 4 MB chip was never read, the head stayed.
   g_rom_flashchip of the bootloader is the RAM copy of the SDK's libspi_flash (0x3ffeed10 in bootloader.map), not the
   record the ROM's SPIRead / SPIWrite / SPIEraseSector check: the ROM's own is set by its SPIParamCfg (as esptool's
   stub does it). 22.11 the Wemos (XMC 16 MB): the package at 0xF95000 failed its CRC on every boot ("package CRC, the
   head stays") - the ROM still had the 4 MB of the image header and refused every read above it */
typedef uint32_t (*espurna_rom_spi_param_cfg_t)(uint32_t device_id, uint32_t chip_size, uint32_t block_size,
    uint32_t sector_size, uint32_t page_size, uint32_t status_mask);
#define ESPURNA_ROM_SPI_PARAM_CFG ((espurna_rom_spi_param_cfg_t)0x40004c2cu)

#ifndef RBL_CHIP_BEGIN
static uint32_t s_chip_size_before;

static void rbl_chip_begin(void) {
    s_chip_size_before = g_rom_flashchip.chip_size;
    const uint32_t capacity = bootloader_read_flash_id() & 0xffu;
    if ((capacity >= 0x14u) && (capacity <= 0x18u)) {
        g_rom_flashchip.chip_size = 1u << capacity;
        ESPURNA_ROM_SPI_PARAM_CFG(0, 1u << capacity, 0x10000u, 0x1000u, 0x100u, 0xffffu);
    }
}

/* the ROM's record back to the size of the image header before the head starts: the head finds the ROM as the SDK's
   bootloader leaves it (the head sets its own size, core_esp8266.cpp flash_size_from_id) */
static void rbl_chip_end(void) {
    g_rom_flashchip.chip_size = s_chip_size_before;
    ESPURNA_ROM_SPI_PARAM_CFG(0, s_chip_size_before, 0x10000u, 0x1000u, 0x100u, 0xffffu);
}

#define RBL_CHIP_BEGIN() rbl_chip_begin()
#define RBL_CHIP_END() rbl_chip_end()
#endif

/* the CPU at 160 MHz while installing (DPORT_CTL_DOUBLE_CLK, as bootloader_start.c with CONFIG_BOOTLOADER_FAST_BOOT);
   ets_printf() only at 80 MHz (the UART divider) */
#ifndef RBL_DPORT_CTL
#define RBL_DPORT_CTL (*(volatile uint32_t*)0x3FF00014u)
#endif
static uint32_t s_clock;
#define RBL_FAST() do { s_clock = RBL_DPORT_CTL; RBL_DPORT_CTL = s_clock | 1u; } while (0)
#define RBL_SLOW() do { RBL_DPORT_CTL = s_clock; } while (0)

/* an image of the flash from 0x0 (ESPURNA_RBL_FLASH, owner 21.11: ESPurna 1.15 back over the air) */
#define RBL_CHIP_FLASH_IMAGE 1

#ifndef RBL_RESET
/* the chip's reset by the hardware watchdog, as the head's restart (core_esp8266.cpp chip::restart()), after the UART had
   its time. Not eboot's SWRST (the RTC's system reset): 21.11 the image of 1.15 was written whole and read back, the
   SWRST that followed left the board silent until a power cycle (the SPI flash controller as the SDK's bootloader set
   it, as after a jump to the ROM's reset vector, 19.11) */
#define RBL_RESET() do { \
        for (volatile uint32_t wait = 0; wait < 200000u; ++wait) {} \
        volatile uint32_t* const wdt = (volatile uint32_t*)0x60000900u; \
        wdt[0] &= ~1u;                                  /* WDT_CTL: off while it is set up */ \
        wdt[1] = 1;                                     /* WDT_OP: the shortest stage */ \
        wdt[2] = 1;                                     /* WDT_OP_ND */ \
        wdt[0] = (wdt[0] & ~0x3Fu) | (7u << 3) | 1u;    /* RSTLEN 7, RSPMOD 0 (reset at once), EN */ \
        for (;;) {} \
    } while (0)
#endif

/* XM25QH..A flash (RDID 20 70 xx, the Wemos D1 mini of 21.11): an XMC chip the SDK's strict list does not know (series
 * 0x40 / 0x41 / 0x50 only), so its XMC startup flow runs, still reads 0x70 and the bootloader reboots ("XMC flash startup
 * fail") before it ever starts a head. That series needs no startup flow: taken as it is. Every other chip goes to the
 * SDK's own check (bootloader_init.c -> --wrap in scripts/esp8266_rtos_sdk.py) */
esp_err_t __real_bootloader_flash_xmc_startup(void);
esp_err_t __wrap_bootloader_flash_xmc_startup(void) {
    const uint32_t id = bootloader_read_flash_id();
    if ((((id >> 16) & 0xffu) == 0x20u) && (((id >> 8) & 0xffu) == 0x70u)) {
        return ESP_OK;
    }
    return __real_bootloader_flash_xmc_startup();
}
