/*

BUZZER of the chip: a square wave of `frequency` on `pin`, its duty 0..255 the loudness (the NSPanel's beeper,
nspanel.cpp). ESP8266 on the RTOS SDK: pins::pwm() (the SDK's pwm driver, one frequency for every pin)

*/

#pragma once

#include "../../pins.h"

#include <cstdint>

namespace espurna {
namespace driver {
namespace buzzer {

inline uint32_t& frequency() {
    static uint32_t out { 1000 };
    return out;
}

inline void setup(int pin, uint32_t value) {
    frequency() = value;
    espurna::pins::mode(pin, espurna::pins::Output);
}

// 8 bit duty
inline void duty(int pin, uint32_t value) {
    espurna::pins::pwm(pin, value, 255, frequency());
}

} // namespace buzzer
} // namespace driver
} // namespace espurna
