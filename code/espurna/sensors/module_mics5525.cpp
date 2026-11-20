/*

SENSOR DRIVER: MICS-5525 CO (analog)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "AnalogSensor.h"
#include "MICS5525Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(mics5525, "mics5525R0|number|R0 (Ohm)|750000;mics5525RL|number|RL (Ohm)|820") {
    auto* sensor = new MICS5525Sensor();
    sensor->setR0(getSetting("mics5525R0", static_cast<unsigned long>(MICS5525_R0)));
    sensor->setRL(getSetting("mics5525RL", static_cast<unsigned long>(MICS5525_RL)));
    sensor->setRS(0);
    sensorAdd(sensor);
}

#endif // MODULE_BUILD
