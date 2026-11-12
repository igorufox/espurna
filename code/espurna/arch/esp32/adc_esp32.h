/*

ADC, ESP32 part (ref. adc_driver.h)

12 bit ADC on GPIO pins. Raw values are not linear, the voltage comes from the calibrated analogReadMilliVolts().
No supply voltage measurement

*/

#pragma once

#include <Arduino.h>

namespace espurna {
namespace adc {
namespace chip {

constexpr int bits() {
    return 12;
}

// full 0..3.3V range
inline void attenuation(uint8_t pin) {
    analogSetPinAttenuation(pin, ADC_11db);
}

inline double millivolts(uint8_t pin, uint32_t) {
    return ::analogReadMilliVolts(pin);
}

inline String address(uint8_t pin) {
    return String(F("GPIO")) + String(pin, 10);
}

inline String pad(uint8_t pin) {
    return address(pin);
}

inline uint16_t vcc() {
    return 0;
}

} // namespace chip
} // namespace adc
} // namespace espurna
