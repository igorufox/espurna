/*

PWM MODULE

Chip part of the PWM, ref. arch/<chip>/pwm_<chip>.h. Only pwm.cpp includes it, every function is inline
(pwm.cpp is a part of the light module, ref. config/head.h ESPURNA_MODULES)

pwm.cpp keeps the channels (pin and duty), the settings, the duty limit and the scale. The chip:
- chip::setup(frequency, resolution)
                         configure the driver; returns the duty of a fully on channel (the range of the values)
- chip::channels_max()   how many pins the driver takes
- chip::pin_valid(pin)   the driver can drive this pin
- chip::init(begin, end) attach these pins (already checked: free, valid, not repeated, not too many), all at 0
- chip::write(index, pin, duty, full)
                         the duty of a channel, applied by update()
- chip::update()         apply all of the written duty values

*/

#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace driver {
namespace pwm {

using StopPwmFunc = void(*)(uint8_t);

// Arduino API analogWrite() (the esp8266 Core or LibreTiny): the pin is updated right away.
// Zero and full duty explicitly stop the PWM of the pin (b/c of a weird dependency on ::digitalWrite implementation),
// the pin simply goes LOW or HIGH
inline void arduino_write(StopPwmFunc stop, uint8_t pin, uint32_t duty, uint32_t full) {
    if (duty == 0) {
        stop(pin);
        digitalWrite(pin, LOW);
    } else if (duty == full) {
        stop(pin);
        digitalWrite(pin, HIGH);
    } else {
        ::analogWrite(pin, duty);
    }
}

} // namespace pwm
} // namespace driver
} // namespace espurna

// arch/<chip>/pwm_chip.h: the chip's folder (-I of its ini)
#include "pwm_chip.h"
