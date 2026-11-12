/*

SENSOR DRIVER: GUVA-S12SD UV (analog)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "GUVAS12SDSensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(guvas12sd, "guvas12sdPin|adc|Pin|") {
    const auto pin = getSetting(F("guvas12sdPin"), uint8_t{ GPIO_NONE });
    if (pin_set(pin)) {
        auto* sensor = new GUVAS12SDSensor();
        sensor->setGPIO(pin);
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
