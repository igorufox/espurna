/*

ADC, esp8266 part (ref. adc_driver.h)

One 10 bit ADC on the TOUT pad (A0), 0..1V. Or the supply voltage, when the build sets ADC_MODE(ADC_VCC)

*/

#pragma once

#include <Arduino.h>

namespace espurna {
namespace adc {
namespace chip {

constexpr int bits() {
    return 10;
}

inline void attenuation(uint8_t) {
}

inline double millivolts(uint8_t, uint32_t raw) {
    return linear_millivolts(raw, bits());
}

inline String address(uint8_t) {
    return F("A0");
}

inline String pad(uint8_t) {
    return F("TOUT");
}

inline uint16_t vcc() {
    return ESP.getVcc();
}

} // namespace chip
} // namespace adc
} // namespace espurna
