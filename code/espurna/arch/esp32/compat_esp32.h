/*

esp8266 Core and SDK API shims, ESP32 part (ref. the common compat_rtos.h, which includes this one)

*/

#pragma once

#include <Arduino.h>
#include <pgmspace.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_system.h>
#if __has_include(<esp_mac.h>)
#include <esp_mac.h>    // ESP-IDF 5 (core 3.x): esp_read_mac() moved out of esp_system.h
#endif
#include <esp_timer.h>
#include <esp_wifi.h>

#define system_get_sdk_version() esp_get_idf_version()
#define system_get_cpu_freq() ((uint8_t)getCpuFrequencyMhz())

// Xtensa cycle counter, same name as the esp8266 Core one
inline uint32_t esp_get_cycle_count() {
    uint32_t ccount;
    __asm__ __volatile__("rsr %0, ccount" : "=a" (ccount));
    return ccount;
}

inline uint64_t micros64() {
    return (uint64_t)esp_timer_get_time();
}

// core 3.x has its own SerialMode (uart_mode_t, HardwareSerial.h)
#if ESP_ARDUINO_VERSION_MAJOR < 3
using SerialMode = uint32_t;
#endif
