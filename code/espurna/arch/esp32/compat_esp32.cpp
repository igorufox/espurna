/*

esp8266 Core API shims, ESP32 part of the ESP object (ref. compat_chip.h)

*/

#include "../../espurna.h"

// system_esp32.cpp: the loop() stall that rebooted the chip last time, empty if none
espurna::string systemStallInfo();

espurna::string EspCompat::getResetReason() {
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "Power On";
    case ESP_RST_EXT: return "External System";
    case ESP_RST_SW: return "Software/System restart";
    case ESP_RST_PANIC: return "Exception";
    case ESP_RST_INT_WDT: return "Interrupt Watchdog";
    case ESP_RST_TASK_WDT: return "Task Watchdog";
    case ESP_RST_WDT: return "Watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep-Sleep Wake";
    case ESP_RST_BROWNOUT: return "Brownout";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_UNKNOWN:
        break;
    }
    return "Unknown";
}

espurna::string EspCompat::getResetInfo() {
    espurna::string out("Reset reason: ");
    out += espurna::string(static_cast<int>(esp_reset_reason()), 10);
    const auto stall = systemStallInfo();
    if (stall.length()) {
        out += ", ";
        out += stall;
    }
    return out;
}

// the boot header of the bootloader (core_esp32.cpp)
uint32_t EspCompat::getFlashChipSpeed() {
    return espurna::core::chip::flash_speed();
}

uint32_t EspCompat::getFlashChipId() {
    return 0;
}

uint32_t EspCompat::getFreeSketchSpace() {
    return espurna::core::chip::sketch_space();
}

uint32_t EspCompat::getSketchSize() {
    return espurna::core::chip::sketch_size();
}

uint32_t EspCompat::getMaxAllocHeap() {
    return espurna::core::chip::heap_max_alloc();
}

bool EspCompat::flashRead(uint32_t, uint32_t*, size_t) {
    return false;
}

