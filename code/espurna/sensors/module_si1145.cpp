/*

SENSOR DRIVER: SI1145 UV, IR and visible light (I2C)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "SI1145Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(si1145, "si1145Addr|i2c|Address (0 - find it)|0") {
    if (i2c_bus()) {
        auto* sensor = new SI1145Sensor();
        sensor->setAddress(getSetting(F("si1145Addr"), uint8_t{ SI1145_ADDRESS }));
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
