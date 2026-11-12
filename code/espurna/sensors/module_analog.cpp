/*

SENSOR DRIVER: Analog input (ADC)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "AnalogSensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(analog, "analogPin|adc|Pin|;analogSamples|number|Samples|10|1-100;analogDelay|number|Delay between samples (us)|0|0-10000;analogFactor|number|Factor|1;analogOffset|number|Offset|0") {
    const auto pin = getSetting(F("analogPin"), uint8_t{ ANALOG_PIN });
    if (pin_set(pin)) {
        auto* sensor = new AnalogSensor();
        sensor->setPin(pin);
        sensor->setSamples(getSetting(F("analogSamples"), ANALOG_SAMPLES));
        sensor->setDelay(getSetting(F("analogDelay"), ANALOG_DELAY));
        sensor->setFactor(getSetting(F("analogFactor"), double{ ANALOG_FACTOR }));
        sensor->setOffset(getSetting(F("analogOffset"), double{ ANALOG_OFFSET }));
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
