/*

esp8266 Core and SDK API shims, BK72xx (BDK) part (ref. the common compat_chip.h, which includes this one)

*/

#pragma once

#include <cstddef>
#include <cstdint>

#include "beken_api.h"

#include <chip_sdk.h>

// FreeRTOS of the SDK, the common code (arch/rtos) takes it from here
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <queue.h>
#include <timers.h>
#include "mutex_beken.h"

// FreeRTOS port interrupt mask (IRQ & FIQ bits of CPSR), nesting-safe and usable from the ISR. Critical section macros
// of the port open a block scope, these do not
extern "C" int port_disable_interrupts_flag(void);
extern "C" void port_enable_interrupts_flag(int);

// Code runs from the flash anyway, nothing to place in IRAM
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

// strings are not in a separate address space
#ifndef printf_P
#define printf_P printf
#endif

// newlib has it, its time.h declares it only with __XSI_VISIBLE (ntp.cpp: the time command)
#include <ctime>
extern "C" char* strptime(const char* s, const char* format, struct tm* tm);

#define system_get_sdk_version() ::espurna::beken::bdk_version()
#define system_get_cpu_freq() ((uint8_t)::espurna::beken::cpu_mhz())

// BK7231N data TCM: the static data placed there (Wi-Fi driver, and ref. scripts/beken_tcm.py), out of its size
struct TcmStats {
    size_t used;
    size_t size;
};

// ref. system_beken.cpp
TcmStats systemTcmStats();

// the 64 bit microseconds of the chip (core_beken.cpp)
inline uint64_t micros64() {
    return ::espurna::core::chip::micros();
}

// Same names as the esp8266 Core / SDK ones (ref. system_time.h)
// FreeRTOS ticks scaled to F_CPU (configCPU_CLOCK_HZ), 2ms resolution
inline uint32_t esp_get_cycle_count() {
    return ::espurna::beken::cpu_cycle_count();
}

// busy wait
inline void ets_delay_us(uint32_t value) {
    ::espurna::core::chip::delay_us(value);
}

#ifndef FLASH_SECTOR_SIZE
#define FLASH_SECTOR_SIZE 4096
#endif

#ifndef SPI_FLASH_SEC_SIZE
#define SPI_FLASH_SEC_SIZE FLASH_SECTOR_SIZE
#endif

using SerialMode = uint32_t;
using SerialConfig = uint16_t;
