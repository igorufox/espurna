/*

SENSOR DRIVER: Dummy sensor (test values)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "DummySensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(dummy, "") {
    sensorAdd(new espurna::sensor::driver::dummy::Sensor());
}

#endif // MODULE_BUILD
