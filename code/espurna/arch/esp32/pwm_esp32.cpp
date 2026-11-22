/*

The PWM of the ESP32 (LEDC): chip_sdk.h pwm_write() / pwm_stop() and the LEDC channels by number of core_esp32.h
(pwm_chip.h). The light_pwm module's, not the head's (config/head.h ESPURNA_MODULES, the custom_modules line of
platformio_esp32.ini), as on every chip; the head and the other modules reach pwm_write() through chip::pwm_bus.

*/

#include "espurna.h"

#if PWM_SUPPORT

#include <chip_sdk.h>

#include "core_esp32.h"

#include <driver/ledc.h>

namespace espurna {
namespace core {
namespace esp32 {
namespace {

// -----------------------------------------------------------------------------
// LEDC: channels 0..15 as Arduino-ESP32 numbers them, 0..7 high speed, 8..15 low speed, two channels per timer
// -----------------------------------------------------------------------------

namespace ledc {

ledc_mode_t mode(uint8_t channel) {
    return (channel < 8) ? LEDC_HIGH_SPEED_MODE : LEDC_LOW_SPEED_MODE;
}

ledc_timer_t timer(uint8_t channel) {
    return static_cast<ledc_timer_t>((channel / 2) % 4);
}

uint8_t bits[16] {};

} // namespace ledc

// the PWM channels of analogWrite() / tone(): from the top, next to the buzzer's (15, buzzer_chip.h); pwm.cpp
// allocates from 0
struct AnalogPin {
    uint8_t pin;
    uint8_t channel;
    uint32_t frequency;
    uint8_t bits;
};

constexpr uint8_t AnalogChannels { 6 };
constexpr uint8_t AnalogFirst { 14 };
AnalogPin analog_pins[AnalogChannels] {};
uint8_t analog_used { 0 };

uint8_t bits_for(uint32_t range) {
    uint8_t out { 1 };
    while ((out < 20) && ((1UL << out) - 1) < range) {
        ++out;
    }

    return out;
}

} // namespace

bool ledc_attach(uint8_t pin, uint8_t channel, uint32_t frequency, uint8_t bits) {
    if (channel >= 16) {
        return false;
    }

    ledc_timer_config_t timer{};
    timer.speed_mode = ledc::mode(channel);
    timer.duty_resolution = static_cast<ledc_timer_bit_t>(bits);
    timer.timer_num = ledc::timer(channel);
    timer.freq_hz = frequency;
    timer.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer) != ESP_OK) {
        return false;
    }

    ledc_channel_config_t config{};
    config.gpio_num = pin;
    config.speed_mode = ledc::mode(channel);
    config.channel = static_cast<ledc_channel_t>(channel % 8);
    config.intr_type = LEDC_INTR_DISABLE;
    config.timer_sel = ledc::timer(channel);
    config.duty = 0;
    config.hpoint = 0;
    if (ledc_channel_config(&config) != ESP_OK) {
        return false;
    }

    ledc::bits[channel] = bits;
    return true;
}

void ledc_write(uint8_t channel, uint32_t duty) {
    if (channel >= 16) {
        return;
    }

    // full duty: all bits set and one more (Arduino-ESP32's ledcWrite() does the same)
    const uint32_t max = (1UL << ledc::bits[channel]) - 1;
    if (duty >= max) {
        duty = max + 1;
    }

    ledc_set_duty(ledc::mode(channel), static_cast<ledc_channel_t>(channel % 8), duty);
    ledc_update_duty(ledc::mode(channel), static_cast<ledc_channel_t>(channel % 8));
}

} // namespace esp32

namespace chip {

using esp32::ledc_attach;
using esp32::ledc_write;
using esp32::analog_pins;
using esp32::analog_used;
using esp32::AnalogChannels;
using esp32::AnalogFirst;
using esp32::AnalogPin;
using esp32::bits_for;

void pwm_write(uint8_t pin, uint32_t duty, uint32_t range, uint32_t frequency) {
    AnalogPin* slot { nullptr };
    for (uint8_t index = 0; index < analog_used; ++index) {
        if (analog_pins[index].pin == pin) {
            slot = &analog_pins[index];
            break;
        }
    }

    if (!slot) {
        if (!duty) {
            return;
        }

        if (analog_used >= AnalogChannels) {
            return;
        }

        slot = &analog_pins[analog_used];
        slot->pin = pin;
        slot->channel = static_cast<uint8_t>(AnalogFirst - analog_used);
        slot->frequency = 0;
        ++analog_used;
    }

    const auto bits = bits_for(range);
    if ((slot->frequency != frequency) || (slot->bits != bits)) {
        if (!ledc_attach(pin, slot->channel, frequency, bits)) {
            return;
        }

        slot->frequency = frequency;
        slot->bits = bits;
    }

    const uint32_t max = (1UL << bits) - 1;
    const uint64_t scaled = range
        ? (static_cast<uint64_t>(duty) * max) / range
        : 0;
    ledc_write(slot->channel, static_cast<uint32_t>(scaled));
}

// the pin back to a plain GPIO: its LEDC channel stopped, the slot free for another pin
void pwm_stop(uint8_t pin) {
    for (uint8_t index = 0; index < analog_used; ++index) {
        if (analog_pins[index].pin == pin) {
            const auto channel = analog_pins[index].channel;
            ledc_stop(esp32::ledc::mode(channel), static_cast<ledc_channel_t>(channel % 8), 0);
            analog_pins[index].frequency = 0;
            return;
        }
    }
}

} // namespace chip
} // namespace core
} // namespace espurna

#endif // PWM_SUPPORT
