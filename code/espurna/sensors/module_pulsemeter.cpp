/*

SENSOR DRIVER: Pulse meter (energy)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "PulseMeterSensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(pulsemeter, "pulsemeterPin|gpio|GPIO|;pulsemeterIntMode|select|Interrupt|2|1=Rising,2=Falling,3=Change;pulsemeterDebounce|number|Debounce (ms)|50|0-1000") {
    const auto pin = getSetting(F("pulsemeterPin"), uint8_t{ PULSEMETER_PIN });
    if (pin_free(pin)) {
        auto* sensor = new PulseMeterSensor();
        sensor->setPin(pin);
        sensor->setInterruptMode(getSetting(F("pulsemeterIntMode"), uint8_t{ PULSEMETER_INTERRUPT_ON }));
        sensor->setDebounceTime(
            PulseMeterSensor::TimeSource::duration{
                getSetting(F("pulsemeterDebounce"), uint32_t{ PULSEMETER_DEBOUNCE })});
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
