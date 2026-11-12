/*

esp8266 Core API shims, ESP32 part of the ESP object (ref. compat_rtos.h)

*/

#include "espurna.h"

#if defined(ESP32)

// system_esp32.cpp: the loop() stall that rebooted the chip last time, empty if none
String systemStallInfo();

String EspCompat::getResetReason() {
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return F("Power On");
    case ESP_RST_EXT: return F("External System");
    case ESP_RST_SW: return F("Software/System restart");
    case ESP_RST_PANIC: return F("Exception");
    case ESP_RST_INT_WDT: return F("Interrupt Watchdog");
    case ESP_RST_TASK_WDT: return F("Task Watchdog");
    case ESP_RST_WDT: return F("Watchdog");
    case ESP_RST_DEEPSLEEP: return F("Deep-Sleep Wake");
    case ESP_RST_BROWNOUT: return F("Brownout");
    case ESP_RST_SDIO: return F("SDIO");
    case ESP_RST_UNKNOWN:
        break;
    }
    return F("Unknown");
}

String EspCompat::getResetInfo() {
    String out(F("Reset reason: "));
    out += String(static_cast<int>(esp_reset_reason()), 10);
    const auto stall = systemStallInfo();
    if (stall.length()) {
        out += F(", ");
        out += stall;
    }
    return out;
}

// the framework EspClass reads them out of the efuse / SPI flash
uint32_t EspCompat::getFlashChipSpeed() {
    return static_cast<EspClass&>(*this).getFlashChipSpeed();
}

uint32_t EspCompat::getFlashChipId() {
    return 0;
}

uint32_t EspCompat::getFreeSketchSpace() {
    return static_cast<EspClass&>(*this).getFreeSketchSpace();
}

uint32_t EspCompat::getSketchSize() {
    return static_cast<EspClass&>(*this).getSketchSize();
}

String EspCompat::getSketchMD5() {
    return static_cast<EspClass&>(*this).getSketchMD5();
}

uint32_t EspCompat::getMaxAllocHeap() {
    return static_cast<EspClass&>(*this).getMaxAllocHeap();
}

bool EspCompat::flashRead(uint32_t, uint32_t*, size_t) {
    return false;
}

#endif
