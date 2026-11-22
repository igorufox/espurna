/*

The PWM of Beken (the BDK's PWM unit, six channels on fixed pins): chip_sdk.h pwm_write() / pwm_stop(). The light_pwm
module's, not the head's (config/head.h ESPURNA_MODULES, the custom_modules line of platformio_beken.ini), as on
every chip; the head (pin_mode() releases a PWM pin) and the other modules reach it through chip::pwm_bus.

*/

#include "espurna.h"

#if PWM_SUPPORT

#include "bdk/bdk.h"

#include <chip_sdk.h>

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace espurna {
namespace core {
namespace chip {

// --- PWM: six channels on fixed pins, 26 MHz clock -------------------------------------------------------------------

namespace {

constexpr uint8_t PwmPins[] { 6, 7, 8, 9, 24, 26 };

int pwm_channel(uint8_t pin) {
    for (size_t index = 0; index < std::size(PwmPins); ++index) {
        if (PwmPins[index] == pin) {
            return static_cast<int>(index);
        }
    }

    return -1;
}

struct Pwm {
    pwm_param_t param;
    bool running;
};

Pwm pwms[std::size(PwmPins)] {};
bool active[std::size(PwmPins)] {};

} // namespace

void pwm_stop(uint8_t pin) {
    const int channel = pwm_channel(pin);
    if ((channel < 0) || !active[channel]) {
        return;
    }

    auto& pwm = pwms[channel];
    if (pwm.running) {
        UINT32 unit = static_cast<UINT32>(channel);
        sddev_control(PWM_DEV_NAME, CMD_PWM_UNIT_DISABLE, &unit);
        pwm.running = false;
    }

    active[channel] = false;
}

void pwm_write(uint8_t pin, uint32_t duty, uint32_t range, uint32_t frequency) {
    const int channel = pwm_channel(pin);
    if ((channel < 0) || !range || !frequency) {
        return;
    }

    auto& pwm = pwms[channel];
    UINT32 unit = static_cast<UINT32>(channel);
    const uint32_t end = (26000000UL / frequency) - 1;
    const uint32_t value = static_cast<uint32_t>((static_cast<uint64_t>(std::min(duty, range)) * end) / range);

    if (!active[channel]) {
        pin_detach(pin);
        active[channel] = true;
        pwm.running = false;
    }

    pwm.param.channel = channel;
    pwm.param.cfg.bits.en = PWM_ENABLE;
    pwm.param.cfg.bits.int_en = PWM_INT_DIS;
    pwm.param.cfg.bits.mode = PWM_PWM_MODE;
    pwm.param.cfg.bits.clk = PWM_CLK_26M;
    pwm.param.end_value = end;
    pwm.param.p_Int_Handler = nullptr;
    pwm.param.duty_cycle1 = value;
    pwm.param.duty_cycle2 = 0;
    pwm.param.duty_cycle3 = 0;

    if (!pwm.running) {
        if (value) {
            sddev_control(PWM_DEV_NAME, CMD_PWM_INIT_PARAM, &pwm.param);
            sddev_control(PWM_DEV_NAME, CMD_PWM_INIT_LEVL_SET_HIGH, &unit);
            sddev_control(PWM_DEV_NAME, CMD_PWM_UNIT_ENABLE, &unit);
            pwm.running = true;
        } else {
            gpio_config(pin, GMODE_OUTPUT);
            gpio_output(pin, 0);
        }
    } else if (value) {
        sddev_control(PWM_DEV_NAME, CMD_PWM_INIT_LEVL_SET_HIGH, &unit);
        sddev_control(PWM_DEV_NAME, CMD_PWM_SET_DUTY_CYCLE, &pwm.param);
    } else {
        sddev_control(PWM_DEV_NAME, CMD_PWM_INIT_LEVL_SET_LOW, &unit);
        sddev_control(PWM_DEV_NAME, CMD_PWM_SET_DUTY_CYCLE, &pwm.param);
        sddev_control(PWM_DEV_NAME, CMD_PWM_UNIT_DISABLE, &unit);
        pwm.running = false;
    }
}

} // namespace chip
} // namespace core
} // namespace espurna

#endif // PWM_SUPPORT
