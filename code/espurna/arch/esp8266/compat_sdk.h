/*

esp8266 Core and SDK API shims, ESP8266 on ESP8266_RTOS_SDK 3.4 (ref. the common compat_chip.h, which includes this one)

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
#include <esp_timer.h>
#include <esp_wifi.h>

// espurna::core::chip::cycle_count()
#include <chip_sdk.h>

#define system_get_sdk_version() esp_get_idf_version()
#define system_get_cpu_freq() ((uint8_t)CONFIG_ESP8266_DEFAULT_CPU_FREQ_MHZ)

// The cycles since the boot, same name as the esp8266 Core one. Not CCOUNT alone: the port's tick sets it back to 0
// every tick (core_esp8266.cpp chip::cycle_count() adds what the port counted; 11-18: the CpuClock of the WebSocket
// queue and the LED patterns ran on a 10 ms counter)
inline uint32_t esp_get_cycle_count() {
    return ::espurna::core::chip::cycle_count();
}

inline uint64_t micros64() {
    return (uint64_t)esp_timer_get_time();
}
