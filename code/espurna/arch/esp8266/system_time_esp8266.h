/*

Part of the SYSTEM MODULE

Clock sources and delays of the esp8266 Core, used by the common system_time.h

*/

#pragma once

#include <Arduino.h>

#include <cstdint>

namespace espurna {
namespace time {
namespace arch {

// `"rsr %0, ccount\n" : "=a" (out) :: "memory"`, 32bit
inline uint32_t cycle_count() {
    return ::esp_get_cycle_count();
}

inline uint64_t micros64() {
    return ::micros64();
}

// busy wait, *without* using SDK or Core timers
inline void delay_us(uint32_t) __attribute__((always_inline));
inline void delay_us(uint32_t value) {
    ::ets_delay_us(value);
}

// SYS tasks are serviced while sleeping
inline void delay(uint32_t ms) {
    ::delay(ms);
}

} // namespace arch
} // namespace time
} // namespace espurna
