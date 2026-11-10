/*

Part of the SYSTEM MODULE, FreeRTOS chips: ESP32 and LibreTiny

Clock sources and delays used by the common system_time.h. The SDK calls are the esp8266 names, given by
arch/<chip>/compat_<chip>.h: esp_get_cycle_count() (ESP32 xtensa ccount, 32bit, wraps every ~18s at 240MHz;
BK72xx FreeRTOS ticks scaled to F_CPU, 2ms resolution), micros64() and ets_delay_us()

*/

#pragma once

#include <Arduino.h>

#include <cstdint>

#include "compat_rtos.h"

namespace espurna {
namespace system {

// Sleep with the loop lock released, so AsyncTCP handlers can run in the
// meantime (same as esp8266 servicing SYS tasks inside delay())
void delay_unlocked(uint32_t ms);

} // namespace system

namespace time {
namespace arch {

inline uint32_t cycle_count() {
    return ::esp_get_cycle_count();
}

// 64bit, never overflows in practice
inline uint64_t micros64() {
    return ::micros64();
}

// busy wait, *without* using SDK or Core timers
inline void delay_us(uint32_t) __attribute__((always_inline));
inline void delay_us(uint32_t value) {
    ::ets_delay_us(value);
}

inline void delay(uint32_t ms) {
    system::delay_unlocked(ms);
}

} // namespace arch
} // namespace time
} // namespace espurna
