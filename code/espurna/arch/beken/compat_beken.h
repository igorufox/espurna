/*

esp8266 Core and SDK API shims, BK72xx (LibreTiny) part (ref. the common compat_chip.h, which includes this one)

*/

#pragma once

#include <Arduino.h>

#include <FreeRTOS.h>
#include <task.h>
#include "mutex_beken.h"

// LibreTiny renames Arduino delay() through a function-like macro (SDK has its own delay()), which
// also hits members and arguments named 'delay'. Arduino.h is already in, the macro is no longer needed
#ifdef delay
#undef delay
inline void delay(unsigned long ms) {
    delayMilliseconds(ms);
}
#endif

// FreeRTOS port interrupt mask (IRQ & FIQ bits of CPSR), nesting-safe and usable from the ISR. Critical section macros
// of the port open a block scope, these do not
extern "C" int port_disable_interrupts_flag(void);
extern "C" void port_enable_interrupts_flag(int);

// Code runs from the flash anyway, nothing to place in IRAM
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

// Print has printf(), strings are not in a separate address space
#ifndef printf_P
#define printf_P printf
#endif

// LibreTiny posix/itoa.c has it, ArduinoCore-API itoa.h is not included by Arduino.h
extern "C" char* ultoa(unsigned long value, char* string, int radix);

// LibreTiny builds avr-libc's dtostrf (cores/common/arduino/src/common/dtostrf.c), no header declares it
extern "C" char* dtostrf(double val, signed char width, unsigned char prec, char* sout);

// Preemptive scheduler, the loop task does not starve anything else
inline void optimistic_yield(uint32_t) {}

#define system_get_sdk_version() lt_get_version()
#define system_get_cpu_freq() ((uint8_t)lt_cpu_get_freq_mhz())

// Same as esp8266 FlashMode_t, LibreTiny does not report the mode (getFlashChipMode() is 0xFF)
enum FlashMode_t {
    FM_QIO = 0x00,
    FM_QOUT = 0x01,
    FM_DIO = 0x02,
    FM_DOUT = 0x03,
    FM_UNKNOWN = 0xff,
};

// BK7231N data TCM: the static data placed there (Wi-Fi driver, and ref. scripts/beken_tcm.py), out of its size
struct TcmStats {
    size_t used;
    size_t size;
};

// ref. system_beken.cpp
TcmStats systemTcmStats();

// micros() extended to 64bit, ref. compat_beken.cpp
uint64_t micros64();

// Same names as the esp8266 Core / SDK ones (ref. system_time.h)
// FreeRTOS ticks scaled to F_CPU (configCPU_CLOCK_HZ), 2ms resolution
inline uint32_t esp_get_cycle_count() {
    return lt_cpu_get_cycle_count();
}

// busy wait
inline void ets_delay_us(uint32_t value) {
    delayMicroseconds(value);
}

#ifndef FLASH_SECTOR_SIZE
#define FLASH_SECTOR_SIZE 4096
#endif

#ifndef SPI_FLASH_SEC_SIZE
#define SPI_FLASH_SEC_SIZE FLASH_SECTOR_SIZE
#endif

using SerialMode = uint32_t;
using SerialConfig = uint16_t;
