/*

BUZZER of the chip: a square wave of `frequency` on `pin`, its duty 0..255 the loudness (the NSPanel's beeper,
nspanel.cpp). ESP32: a LEDC channel of its own (pwm.cpp allocates them from 0)

*/

#pragma once

#include "core_esp32.h"

#include <cstdint>

namespace espurna {
namespace driver {
namespace buzzer {

constexpr uint8_t Channel { 15 };

inline void setup(int pin, uint32_t frequency) {
    ::espurna::core::esp32::ledc_attach(static_cast<uint8_t>(pin), Channel, frequency, 8);
}

inline void duty(int, uint32_t value) {
    ::espurna::core::esp32::ledc_write(Channel, value);
}

} // namespace buzzer
} // namespace driver
} // namespace espurna
