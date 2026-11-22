/*

The RAM of the ESP32's Bluetooth controller in ROM, kept by the head for the ble module (owner 2026-11-19, rtos-arch:
nothing lost while the module is off, the module starts after a reboot, which the Apply always does).

The controller in ROM writes into fixed places of the internal RAM (esp_bt.h, the same on every revision of the chip):
its .data (SOC_MEM_BT_DATA 0x3ffae6e0, 6 KB), the exchange memory of the radio (SOC_MEM_BT_EM 0x3ffb0000, BLE 10 KB
+ BR/EDR), its .bss (SOC_MEM_BT_BSS 0x3ffb8000, 6.5 KB) and a word or so (SOC_MEM_BT_MISC 0x3ffbdb28). With
CONFIG_BT_ENABLED the link script starts the head's data behind all of it (CONFIG_BTDM_RESERVE_DRAM 0xdb5c); the
places between them are the heap's from the start. ESP-IDF's bt.c reserves the four regions
(SOC_RESERVE_MEMORY_REGION rom_bt_*); bt.c is in the head (one copy of the controller's state for the head and the ble
module), held there by the call below. They are given to the heap at boot when the ble module is not on: then the
controller never runs this boot. (22.11: the head reserved them a second time itself, the two equal regions failed
the assert of s_prepare_reserved_regions before app_main on every ESP32.)

When the module is on, they stay its; nimble_port_init() of the module gives the BR/EDR part of the exchange memory
to the heap itself (esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)): the BLE controller does not use it.

*/

#include <sdkconfig.h>

#if CONFIG_BT_ENABLED

#include "../../espurna.h"

#if !MODULE_BUILD

#include "../../modules.h"

#include <esp_bt.h>
#include <esp_rom_sys.h>
#include <esp_heap_caps_init.h>

namespace {

bool kept { false };

void give(intptr_t start, intptr_t end) {
    const auto err = heap_caps_add_region(start, end);
    // too small for a heap of its own (the misc words): nothing to give
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_SIZE)) {
        esp_rom_printf("[BLE] heap_caps_add_region 0x%08x..0x%08x: %d\n", start, end, err);
    }
}

} // namespace

// app_main(), before the loop task: the ble module of the directory decides who gets the RAM of the controller
void bleRamSetup() {
    kept = modulesEnabled("ble");
    if (kept) {
        return;
    }

    // bt.c of the head reserved the regions; a controller that is not idle (never at boot) keeps them
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE) {
        return;
    }

    give(SOC_MEM_BT_DATA_START, SOC_MEM_BT_DATA_END);
    give(SOC_MEM_BT_EM_START, SOC_MEM_BT_EM_BREDR_REAL_END);
    give(SOC_MEM_BT_BSS_START, SOC_MEM_BT_BSS_END);
    give(SOC_MEM_BT_MISC_START, SOC_MEM_BT_MISC_END);
}

// the ble module asks before it starts the controller: false after an Apply that switched it on without a reboot
bool bleRamKept() {
    return kept;
}

#endif // !MODULE_BUILD

#endif // CONFIG_BT_ENABLED
