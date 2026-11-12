/*

PWM MODULE, ESP32 part (ref. pwm_driver.h)

LEDC, one LEDC channel per pin, channel numbers are the indexes of the pins

*/

#pragma once

#include <Arduino.h>

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

// SOC_LEDC_CHANNEL_NUM: classic ESP32 = 16, S2/S3 = 8, C3 = 6.
// pwm.cpp bails before touching the peripheral so a misconfigured pin list
// cannot half-claim channels and leak them.
inline size_t channels_max() {
    return SOC_LEDC_CHANNEL_NUM;
}

inline bool pin_valid(uint8_t pin) {
    return gpioValidForOutput(pin);
}

// core 3.x: ledcSetup() / ledcAttachPin() are gone, ledcAttachChannel() keeps the explicit channel numbers
inline bool init(const uint8_t* begin, const uint8_t* end) {
    const auto& config = ledc::config();

    uint8_t channel = 0;
    for (auto it = begin; it != end; ++it) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcAttachChannel(*it, config.frequency, config.resolution, channel);
#else
        ledcSetup(channel, config.frequency, config.resolution);
        ledcAttachPin(*it, channel);
#endif
        ++channel;
    }

    return true;
}

inline void write(size_t index, uint8_t, uint32_t duty, uint32_t) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWriteChannel(index, duty);  // ledcWrite() takes the pin in 3.x
#else
    ledcWrite(index, duty);
#endif
}

inline void update() {
}

} // namespace chip
} // namespace pwm
} // namespace driver
} // namespace espurna
