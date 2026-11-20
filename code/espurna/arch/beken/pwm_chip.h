/*

PWM MODULE, BK72xx part (ref. pwm_driver.h)

chip::pwm_write() (core_beken.cpp, the PWM of the BDK): one frequency for every pin, resolution in bits

*/

#pragma once

#include "../../pins.h"

#include <limits>

namespace espurna {
namespace driver {
namespace pwm {
namespace chip {

struct Config {
    uint32_t frequency;
    uint32_t range;
};

inline Config& config() {
    static Config out{};
    return out;
}

// the PWM of a pin stops with a duty of 0, espurna::pins::write() sets the level
inline void stop_duty(uint8_t pin) {
    espurna::pins::pwm(pin, 0, config().range, config().frequency);
}

inline void duty(uint8_t pin, uint32_t value) {
    espurna::pins::pwm(pin, value, config().range, config().frequency);
}

inline uint32_t setup(uint32_t frequency, uint32_t resolution) {
    config() = Config{
        .frequency = frequency,
        .range = (1UL << resolution) - 1,
    };

    DEBUG_MSG_P(PSTR("[PWM] BK72xx - Frequency %u (Hz), resolution %u (bits)\n"),
        frequency, resolution);
    return config().range;
}

inline size_t channels_max() {
    return std::numeric_limits<size_t>::max();
}

inline bool pin_valid(uint8_t) {
    return true;
}

// pwm_write() configures the pin
inline bool init(const uint8_t*, const uint8_t*) {
    return true;
}

inline void write(size_t, uint8_t pin, uint32_t duty, uint32_t full) {
    arduino_write(stop_duty, chip::duty, pin, duty, full);
}

inline void update() {
}

} // namespace chip
} // namespace pwm
} // namespace driver
} // namespace espurna
