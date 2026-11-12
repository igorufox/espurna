/*

ADC, LibreTiny part (ref. adc_driver.h)

analogRead() of the core, 10 bit, values taken as the esp8266 0..1V range. No supply voltage measurement

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
    return 0;
}

} // namespace chip
} // namespace adc
} // namespace espurna
