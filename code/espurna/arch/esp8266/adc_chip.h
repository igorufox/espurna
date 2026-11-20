/*

ADC, ESP8266 on ESP8266_RTOS_SDK (ref. adc_driver.h)

The one TOUT pad (A0), 10 bit, 0..1 V, espurna::pins::analog() of the layer (the SDK's adc_read()). No supply voltage measurement
(the SDK measures either TOUT or VDD33, set at build time)

*/

#pragma once


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

inline espurna::string address(uint8_t) {
    return "A0";
}

inline espurna::string pad(uint8_t) {
    return "TOUT";
}

inline uint16_t vcc() {
    return 0;
}

} // namespace chip
} // namespace adc
} // namespace espurna
