// ESP of the layer: the part of the EspClass of the ESP8266 / ESP32 cores that ESPurna (arch/rtos/compat_chip.h)
// and the libraries (IRremoteESP8266: ESP.restart()) call, once for every chip over core_chip.h
#pragma once

#include <stddef.h>
#include <stdint.h>

// the flash modes of the ESP cores (getFlashChipMode(), the boot header of the bootloader)
typedef enum {
    FM_QIO = 0x00,
    FM_QOUT = 0x01,
    FM_DIO = 0x02,
    FM_DOUT = 0x03,
    FM_FAST_READ = 0x04,
    FM_SLOW_READ = 0x05,
    FM_UNKNOWN = 0xff,
} FlashMode_t;

class EspClass {
public:
    uint32_t getFreeHeap();
    uint32_t getMaxAllocHeap();

    uint32_t getFlashChipSize();
    uint32_t getFlashChipMode();
    uint32_t getFlashChipSpeed();

    uint32_t getSketchSize();
    uint32_t getFreeSketchSpace();

    uint32_t getCycleCount();
    uint32_t getCpuFreqMHz();

    const char* getSdkVersion();
    const char* getChipModel();
    uint8_t getChipRevision();

    [[noreturn]] void restart();
};

extern EspClass ESP;
