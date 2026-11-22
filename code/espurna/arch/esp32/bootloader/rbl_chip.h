/*

The ESP32 part of the bootloader step (arch/rtos/bootloader/espurna_rbl.c): what ESP-IDF's bootloader forces, nothing of
the install itself. Included once, by espurna_rbl.c (code/bootloader_components/espurna_rbl, its include folder)

- the layout: ../rbl_esp32.h
- the RTC watchdog of the bootloader (CONFIG_BOOTLOADER_WDT_TIME_MS, 9 s) fed between the sectors: a swap takes longer
- the buffers in RAM the bootloader does not use while the step runs: its own dram_seg is 24 KB, the buffers ~28 KB. The
  app's DRAM (SRAM2 0x3FFB8000..) is free until the bootloader loads the app, after this step
- the flash unlocked first (RBL_CHIP_BEGIN): the bootloader's erase does not unlock it
- bootloader_hooks_include(): the bootloader's main links it (-u), so this component is in the link

*/

#pragma once

#include <stdint.h>

#include "esp_rom_sys.h"
#include "hal/wdt_hal.h"

#include "../rbl_esp32.h"

#define RBL_PRINTF esp_rom_printf

/* where a step of the swap fails, on the UART (22.11: found the 256-sector tables this way); ~400 bytes of the room */
#define RBL_TRACE(...) esp_rom_printf(__VA_ARGS__)

#define RBL_RAM 0x3FFB8000u

static inline void rbl_feed(void) {
    wdt_hal_context_t rtc = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_write_protect_disable(&rtc);
    wdt_hal_feed(&rtc);
    wdt_hal_write_protect_enable(&rtc);
}

#define RBL_FEED() rbl_feed()

/* the flash's write protection off before the step: ESP-IDF's bootloader_flash_write() unlocks it, its
   bootloader_flash_erase_sector() does not (22.11 the DevKit: the first erase of the swap failed, "install failed, again
   on the next boot") */
esp_err_t bootloader_flash_unlock(void);
#define RBL_CHIP_BEGIN() bootloader_flash_unlock()

/* ESP-IDF 5 checks an image without loading it by esp_image_verify() */
#define RBL_IMAGE_CHECK(part, data) esp_image_verify(ESP_IMAGE_VERIFY_SILENT, (part), (data))

/* an image of the flash from 0x0 (ESPURNA_RBL_FLASH, owner 22.11: the migration both ways as on the ESP8266,
   scripts/esp32_migrate.py): the safeboot layout of the Arduino builds and back */
#define RBL_CHIP_FLASH_IMAGE 1

/* after an image of the flash: the chip's reset once the UART had its time; the ROM starts the bootloader the image
   brought (ESP-IDF's bootloader_reset(): the RTC's system reset) */
__attribute__((__noreturn__)) void bootloader_reset(void);    /* bootloader_utility.h, a private header */
#define RBL_RESET() do { \
        esp_rom_delay_us(20000); \
        bootloader_reset(); \
        for (;;) {} \
    } while (0)

void bootloader_hooks_include(void) {
}
