/*

ADC

Chip part of the analog input, ref. arch/<chip>/adc_<chip>.h. Every function is inline
(the analog sensors are a part of the sensors module, ws.cpp of the head)

- adc::chip::bits()                 resolution of analogRead()
- adc::chip::attenuation(pin)       before the first analogRead() of the pin, its full voltage range
- adc::chip::millivolts(pin, raw)   voltage at the pin, `raw` is the analogRead() just taken
- adc::chip::address(pin)           sensor address of the pin ("A0", "GPIO34")
- adc::chip::pad(pin)               sensor description of the pin ("TOUT", "GPIO34")
- adc::chip::vcc()                  supply voltage in mV, only with ADC_MODE_VALUE == ADC_VCC

*/

#pragma once

#include <Arduino.h>

#include <cstdint>

namespace espurna {
namespace adc {

// ADC of the 0..1V range, raw values are linear
constexpr double linear_millivolts(uint32_t raw, int bits) {
    return (static_cast<double>(raw) * 1000.0) / static_cast<double>((1 << bits) - 1);
}

} // namespace adc
} // namespace espurna

// arch/<chip>/adc_chip.h: the chip's folder (-I of its ini)
#include "adc_chip.h"
