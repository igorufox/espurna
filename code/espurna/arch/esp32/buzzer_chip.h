/*

BUZZER of the chip: a square wave of `frequency` on `pin`, its duty 0..255 the loudness (the NSPanel's beeper,
nspanel.cpp). ESP32: a LEDC channel of its own (pwm.cpp allocates them from 0)

*/

#pragma once

#include <Arduino.h>

#include <cstdint>

namespace espurna {
namespace driver {
namespace buzzer {

constexpr uint8_t Channel { 15 };

inline void setup(int pin, uint32_t frequency) {
    ledcAttachChannel(pin, frequency, 8, Channel);
}

inline void duty(int, uint32_t value) {
    ledcWriteChannel(Channel, value);
}

} // namespace buzzer
} // namespace driver
} // namespace espurna
