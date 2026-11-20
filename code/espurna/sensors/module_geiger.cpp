/*

SENSOR DRIVER: Geiger counter
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "GeigerSensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(geiger, "geigerPin|gpio|GPIO|;geigerDebounce|number|Debounce (ms)|25|0-1000;geigerCpm2Sv|number|CPM per uSv/h|240") {
    const auto pin = getSetting("geigerPin", uint8_t{ GEIGER_PIN });
    if (pin_free(pin)) {
        auto* sensor = new GeigerSensor();
        sensor->setGPIO(pin);
        sensor->setMode(GEIGER_PIN_MODE);
        sensor->setDebounceTime(
            GeigerSensor::TimeSource::duration { getSetting("geigerDebounce", uint32_t{ GEIGER_DEBOUNCE }) });
        sensor->setInterruptMode(GEIGER_INTERRUPT_MODE);
        sensor->setCPM2SievertFactor(getSetting("geigerCpm2Sv", GEIGER_CPM2SIEVERT));
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
