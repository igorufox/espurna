/*

SENSOR DRIVER: MICS-2710 NO2 (analog)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "AnalogSensor.h"
#include "MICS2710Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(mics2710, "mics2710Pre|gpio|Pre-heat GPIO|;mics2710R0|number|R0 (Ohm)|2200;mics2710RL|number|RL (Ohm)|820") {
    const auto pre = getSetting("mics2710Pre", uint8_t{ GPIO_NONE });
    if (pin_free(pre)) {
        auto* sensor = new MICS2710Sensor();
        sensor->setPreHeatGPIO(pre);
        sensor->setR0(getSetting("mics2710R0", static_cast<unsigned long>(MICS2710_R0)));
        sensor->setRL(getSetting("mics2710RL", static_cast<unsigned long>(MICS2710_RL)));
        sensor->setRS(0);
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
