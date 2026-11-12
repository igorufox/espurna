/*

PWM MODULE, LibreTiny part (ref. pwm_driver.h)

analogWrite() of the core, the same API as the esp8266 Core with its own names, resolution in bits

*/

#pragma once

#include <Arduino.h>

#include <limits>

namespace espurna {
namespace driver {
namespace pwm {
namespace chip {

// the PWM of a pin stops with a duty of 0, digitalWrite() sets the level
inline void stop_duty(uint8_t pin) {
    ::analogWrite(pin, 0);
}

inline uint32_t setup(uint32_t frequency, uint32_t resolution) {
    ::analogWriteResolution(resolution);
    ::analogWriteFrequency(frequency);

    DEBUG_MSG_P(PSTR("[PWM] Arduino - Frequency %u (Hz), resolution %u (bits)\n"),
        frequency, resolution);
    return (1UL << resolution) - 1;
}

inline size_t channels_max() {
    return std::numeric_limits<size_t>::max();
}

inline bool pin_valid(uint8_t) {
    return true;
}

// analogWrite() configures the pin
inline bool init(const uint8_t*, const uint8_t*) {
    return true;
}

inline void write(size_t, uint8_t pin, uint32_t duty, uint32_t full) {
    arduino_write(stop_duty, pin, duty, full);
}

inline void update() {
}

} // namespace chip
} // namespace pwm
} // namespace driver
} // namespace espurna
