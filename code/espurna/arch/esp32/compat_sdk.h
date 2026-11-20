/*

esp8266 Core and SDK API shims, ESP32 part (ref. the common compat_chip.h, which includes this one)

*/

#pragma once

#include "../../progmem.h"

// FreeRTOS of the SDK, the common code (arch/rtos) takes it from here
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <freertos/timers.h>

#include <esp_system.h>
#if __has_include(<esp_mac.h>)
#include <esp_mac.h>    // ESP-IDF 5 (core 3.x): esp_read_mac() moved out of esp_system.h
#endif
#include <esp_timer.h>
#include <esp_private/esp_clk.h>
#include <esp_wifi.h>

// ets_delay_us() of system_time.h, a ROM function
#include <esp32/rom/ets_sys.h>

// the CPU clock (system_time.h ClockCycles), sdkconfig.defaults: 240 MHz
#ifndef F_CPU
#define F_CPU (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000UL)
#endif

#define system_get_sdk_version() esp_get_idf_version()
#define system_get_cpu_freq() ((uint8_t)(esp_clk_cpu_freq() / 1000000))

// Xtensa cycle counter, same name as the esp8266 Core one
inline uint32_t esp_get_cycle_count() {
    uint32_t ccount;
    __asm__ __volatile__("rsr %0, ccount" : "=a" (ccount));
    return ccount;
}

inline uint64_t micros64() {
    return (uint64_t)esp_timer_get_time();
}
