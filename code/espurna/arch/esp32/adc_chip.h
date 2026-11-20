/*

ADC, ESP32 part (ref. adc_driver.h)

12 bit ADC on GPIO pins. Raw values are not linear, the voltage comes from the calibrated core::esp32::adc_millivolts().
No supply voltage measurement

*/

#pragma once

#include "core_esp32.h"

namespace espurna {
namespace adc {
namespace chip {

constexpr int bits() {
    return 12;
}

// full 0..3.3V range: every channel is set up with 11 dB (core_esp32.cpp)
inline void attenuation(uint8_t) {
}

inline double millivolts(uint8_t pin, uint32_t) {
    return ::espurna::core::esp32::adc_millivolts(pin);
}

inline espurna::string address(uint8_t pin) {
    return espurna::string("GPIO") + espurna::string(pin, 10);
}

inline espurna::string pad(uint8_t pin) {
    return address(pin);
}

inline uint16_t vcc() {
    return 0;
}

} // namespace chip
} // namespace adc
} // namespace espurna
