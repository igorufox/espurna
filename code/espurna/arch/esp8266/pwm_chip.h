/*

PWM MODULE, esp8266 part (ref. pwm_driver.h)

Selected at build time (PWM_PROVIDER):
- PWM_PROVIDER_GENERIC, new-pwm (arch/esp8266/pwm_esp8266.c), a drop-in replacement for the version provided in
  the Espressif SDK. Up to 8 channels on GPIO0..15, duty in units of 200ns of the period
- PWM_PROVIDER_ARDUINO, analogWrite() of the Core. Either PWM or Phase locked, selected at build time.
  Default is PWM locked, enable Phase locked one by adding `enablePhaseLockedWaveform();` somewhere in the code.
  Core declares both implementations as a standalone function /w `__attribute__((weak))`, linker will prioritize
  one over the other when symbol from a specific `.cpp.o` is used

*/

#pragma once

#include <Arduino.h>

#include <array>
#include <chrono>
#include <limits>
#include <vector>

#if PWM_PROVIDER == PWM_PROVIDER_GENERIC
#include "libs/esp8266_pwm.h"
#elif PWM_PROVIDER == PWM_PROVIDER_ARDUINO
// We need to explicitly perform a 'stop' for the PIN, otherwise it will stay on the minimal duty value
// This is only available in Core 3.x.x
extern "C" bool _stopPWM(uint8_t pin) __attribute__((weak));
// This is available in both versions, but we prefer the above
extern "C" int stopWaveform(uint8_t pin);
#endif

namespace espurna {
namespace driver {
namespace pwm {
namespace chip {

#if PWM_PROVIDER == PWM_PROVIDER_GENERIC
namespace generic {

// TODO: peripherals header should expose our register accessor funcs?
// (or just move these to the lib itself?)

static constexpr std::array<uint32_t, 16> addr PROGMEM {
    PERIPHS_IO_MUX_GPIO0_U,    // GPIO0
    PERIPHS_IO_MUX_U0TXD_U,    // GPIO1
    PERIPHS_IO_MUX_GPIO2_U,    // GPIO2
    PERIPHS_IO_MUX_U0RXD_U,    // GPIO3
    PERIPHS_IO_MUX_GPIO4_U,    // GPIO4
    PERIPHS_IO_MUX_GPIO5_U,    // GPIO5
    PERIPHS_IO_MUX_SD_CLK_U,   // GPIO6  **UNUSED**
    PERIPHS_IO_MUX_SD_DATA0_U, // GPIO7  **UNUSED**
    PERIPHS_IO_MUX_SD_DATA1_U, // GPIO8  **UNUSED**
    PERIPHS_IO_MUX_SD_DATA2_U, // GPIO9
    PERIPHS_IO_MUX_SD_DATA3_U, // GPIO10
    PERIPHS_IO_MUX_SD_CMD_U,   // GPIO11 **UNUSED**
    PERIPHS_IO_MUX_MTDI_U,     // GPIO12
    PERIPHS_IO_MUX_MTCK_U,     // GPIO13
    PERIPHS_IO_MUX_MTMS_U,     // GPIO14
    PERIPHS_IO_MUX_MTDO_U,     // GPIO15
};

static constexpr std::array<uint32_t, 16> func PROGMEM {
    FUNC_GPIO0,
    FUNC_GPIO1,
    FUNC_GPIO2,
    FUNC_GPIO3,
    FUNC_GPIO4,
    FUNC_GPIO5,
    FUNC_GPIO6,
    FUNC_GPIO7,
    FUNC_GPIO8,
    FUNC_GPIO9,
    FUNC_GPIO10,
    FUNC_GPIO11,
    FUNC_GPIO12,
    FUNC_GPIO13,
    FUNC_GPIO14,
    FUNC_GPIO15,
};

// > By default there is one small difference to the SDK. The code uses a unit of 200ns for both period and duty.
// > E.g. for 10% duty cycle at 1kHz you need to specify a period value of 5000 and a duty cycle value of 500,
// > a duty cycle of 5000 or above switches the channel to full on.
constexpr uint32_t Step { 200 }; // nanoseconds

constexpr uint32_t period(uint32_t frequency) {
    return std::nano::den / frequency / Step;
}

inline uint32_t& current_period() {
    static uint32_t out { 0 };
    return out;
}

} // namespace generic

inline uint32_t setup(uint32_t frequency, uint32_t) {
    const auto period = generic::period(frequency);
    generic::current_period() = period;
    DEBUG_MSG_P(PSTR("[PWM] Generic - Frequency %u (Hz), period %u (ns)\n"), frequency, period);
    return period;
}

inline size_t channels_max() {
    return 8;
}

inline bool pin_valid(uint8_t pin) {
    return (pin < generic::addr.size())
        && (pin < generic::func.size());
}

inline bool init(const uint8_t* begin, const uint8_t* end) {
    std::vector<pwm_pin_info> channels;
    for (auto it = begin; it != end; ++it) {
        const auto pin = *it;
        pinMode(pin, OUTPUT);
        channels.push_back(pwm_pin_info{
            .addr = generic::addr[pin],
            .func = generic::func[pin],
            .pin = pin,
        });
    }

    ::pwm_init(generic::current_period(), nullptr,
        channels.size(), channels.data());

    constexpr uint32_t Initial = 0;
    for (uint32_t channel = 0; channel < channels.size(); ++channel) {
        ::pwm_set_duty(Initial, channel);
    }

    ::pwm_start();

    return true;
}

inline void write(size_t index, uint8_t, uint32_t duty, uint32_t) {
    ::pwm_set_duty(duty, index);
}

inline void update() {
    ::pwm_start();
}

#elif PWM_PROVIDER == PWM_PROVIDER_ARDUINO
namespace arduino {

inline void stop_waveform(uint8_t pin) {
    stopWaveform(pin);
}

inline void stop_pwm(uint8_t pin) {
    _stopPWM(pin);
}

inline StopPwmFunc& stop() {
    static StopPwmFunc out { nullptr };
    return out;
}

} // namespace arduino

inline uint32_t setup(uint32_t frequency, uint32_t resolution) {
    const uint32_t full = (1UL << resolution) - 1;
    ::analogWriteRange(full);
    ::analogWriteFreq(frequency);

    // Handle 2.7.4 -> 3.x.x migration
    arduino::stop() =
        (_stopPWM)
            ? arduino::stop_pwm
            : arduino::stop_waveform;

    DEBUG_MSG_P(PSTR("[PWM] Arduino - Frequency %u (Hz), resolution %u (bits)\n"),
        frequency, resolution);
    return full;
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
    arduino_write(arduino::stop(), pin, duty, full);
}

inline void update() {
}

#endif

} // namespace chip
} // namespace pwm
} // namespace driver
} // namespace espurna
