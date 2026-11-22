/*

LIGHT_PWM MODULE, ESP8266 on the RTOS SDK: chip::pwm_write() of arch/rtos/chip_sdk.h on the PWM driver of the SDK.
In the light_pwm module only (custom_modules of platformio_esp8266.ini), the head has no PWM

*/

#include "espurna.h"

#if PWM_SUPPORT

#include <chip_sdk.h>

#include <freertos/FreeRTOS.h>

#include <driver/pwm.h>

#include <string.h>

namespace espurna {
namespace core {
namespace esp8266 {
namespace {

// -----------------------------------------------------------------------------
// PWM: the SDK driver takes every channel at once (one period), a new pin or frequency starts it again
// -----------------------------------------------------------------------------

constexpr size_t PwmChannels { 8 };

uint32_t pwm_pins[PwmChannels] {};
uint32_t pwm_duties[PwmChannels] {};
size_t pwm_used { 0 };
uint32_t pwm_period { 0 };
bool pwm_running { false };

void pwm_restart() {
    if (pwm_running) {
        pwm_stop(0);
        pwm_deinit();
        pwm_running = false;
    }

    if (!pwm_used || !pwm_period) {
        return;
    }

    if (pwm_init(pwm_period, pwm_duties, static_cast<uint8_t>(pwm_used), pwm_pins) == ESP_OK) {
        pwm_running = (pwm_start() == ESP_OK);
    }
}

} // namespace
} // namespace esp8266

namespace chip {

using namespace esp8266;

// the output off (duty 0), its channel stays the pin's until a restart of the SDK's driver
void pwm_stop(uint8_t pin) {
    pwm_write(pin, 0, 1, 1000);
}

void pwm_write(uint8_t pin, uint32_t duty, uint32_t range, uint32_t frequency) {
    if (!frequency) {
        return;
    }

    const uint32_t period = 1000000 / frequency;

    size_t index = 0;
    for (; index < pwm_used; ++index) {
        if (pwm_pins[index] == pin) {
            break;
        }
    }

    const uint32_t value = range
        ? static_cast<uint32_t>((static_cast<uint64_t>(duty) * period) / range)
        : 0;

    if (index == pwm_used) {
        if (!duty || (pwm_used >= PwmChannels)) {
            return;
        }

        pwm_pins[pwm_used] = pin;
        pwm_duties[pwm_used] = value;
        ++pwm_used;
        pwm_period = period;
        pwm_restart();
        return;
    }

    if (period != pwm_period) {
        pwm_duties[index] = value;
        pwm_period = period;
        pwm_restart();
        return;
    }

    pwm_duties[index] = value;
    if (pwm_running) {
        pwm_set_duty(static_cast<uint8_t>(index), value);
        pwm_start();
    }
}

} // namespace chip
} // namespace core
} // namespace espurna

#endif
