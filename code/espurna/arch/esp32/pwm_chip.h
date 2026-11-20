/*

PWM MODULE, ESP32 part (ref. pwm_driver.h)

LEDC, one LEDC channel per pin, channel numbers are the indexes of the pins

*/

#pragma once

#include "core_esp32.h"

namespace espurna {
namespace driver {
namespace pwm {
namespace chip {
namespace ledc {

struct Config {
    uint32_t frequency;
    uint32_t resolution;
};

inline Config& config() {
    static Config out{};
    return out;
}

} // namespace ledc

inline uint32_t setup(uint32_t frequency, uint32_t resolution) {
    ledc::config() = ledc::Config{
        .frequency = frequency,
        .resolution = resolution,
    };

    DEBUG_MSG_P(PSTR("[PWM] ESP32 LEDC - Frequency %u (Hz), resolution %u (bits)\n"),
        frequency, resolution);
    return (1UL << resolution) - 1;
}

// the 16 LEDC channels of the ESP32, 8 high speed and 8 low speed (core_esp32.h numbering).
// pwm.cpp bails before touching the peripheral so a misconfigured pin list
// cannot half-claim channels and leak them.
inline size_t channels_max() {
    return 16;
}

inline bool pin_valid(uint8_t pin) {
    return gpioValidForOutput(pin);
}

// LEDC channels by number, the index of the pin is its channel (core_esp32.h)
inline bool init(const uint8_t* begin, const uint8_t* end) {
    const auto& config = ledc::config();

    uint8_t channel = 0;
    for (auto it = begin; it != end; ++it) {
        ::espurna::core::esp32::ledc_attach(*it, channel, config.frequency, config.resolution);
        ++channel;
    }

    return true;
}

inline void write(size_t index, uint8_t, uint32_t duty, uint32_t) {
    ::espurna::core::esp32::ledc_write(static_cast<uint8_t>(index), duty);
}

inline void update() {
}

} // namespace chip
} // namespace pwm
} // namespace driver
} // namespace espurna
