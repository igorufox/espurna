/*

BUZZER of the chip: a square wave of `frequency` on `pin`, its duty 0..255 the loudness (the NSPanel's beeper,
nspanel.cpp). Beken: analogWrite() of LibreTiny (the PWM of the chip, one frequency for every pin)

*/

#pragma once

#include <Arduino.h>

#include <cstdint>

namespace espurna {
namespace driver {
namespace buzzer {

inline void setup(int pin, uint32_t frequency) {
    analogWriteResolution(8);
    analogWriteFrequency(frequency);
    pinMode(pin, OUTPUT);
}

inline void duty(int pin, uint32_t value) {
    analogWrite(pin, value);
}

} // namespace buzzer
} // namespace driver
} // namespace espurna
