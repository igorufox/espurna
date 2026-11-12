/*

SENSOR DRIVER: NTC thermistor (analog)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "NTCSensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(ntc, "ntcPin|adc|Pin|;ntcBeta|number|Beta|3977;ntcRUp|number|Resistor up (Ohm)|0;ntcRDown|number|Resistor down (Ohm)|10000;ntcR0|number|R0 (Ohm)|10000") {
    const auto pin = getSetting(F("ntcPin"), uint8_t{ NTC_PIN });
    if (pin_set(pin)) {
        auto* sensor = new NTCSensor();
        sensor->setPin(pin);
        sensor->setSamples(NTC_SAMPLES);
        sensor->setDelay(NTC_DELAY);
        sensor->setUpstreamResistor(getSetting(F("ntcRUp"), static_cast<unsigned long>(NTC_R_UP)));
        sensor->setDownstreamResistor(getSetting(F("ntcRDown"), static_cast<unsigned long>(NTC_R_DOWN)));
        sensor->setInputVoltage(NTC_INPUT_VOLTAGE);
        sensor->setBeta(getSetting(F("ntcBeta"), static_cast<unsigned long>(NTC_BETA)));
        sensor->setR0(getSetting(F("ntcR0"), static_cast<unsigned long>(NTC_R0)));
        sensor->setT0(NTC_T0);
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
