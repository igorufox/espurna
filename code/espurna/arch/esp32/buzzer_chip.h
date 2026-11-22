/*

BUZZER of the chip: a square wave of `frequency` on `pin`, its duty 0..255 the loudness (the NSPanel's beeper,
nspanel.cpp). ESP32: pins::pwm() (the LEDC of the light_pwm module, chip_sdk.h PwmBus), as on every chip

*/

#pragma once

#include "../../pins.h"

#include <cstdint>

namespace espurna {
namespace driver {
namespace buzzer {

inline uint32_t& frequency() {
    static uint32_t out { 2700 };
    return out;
}

inline void setup(int pin, uint32_t value) {
    frequency() = value;
    espurna::pins::mode(static_cast<uint8_t>(pin), espurna::pins::Output);
}

inline void duty(int pin, uint32_t value) {
    espurna::pins::pwm(static_cast<uint8_t>(pin), value, 255, frequency());
}

} // namespace buzzer
} // namespace driver
} // namespace espurna
