/*

SENSOR DRIVER: SI7021 / HTU21D temperature and humidity (I2C)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "SI7021Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(si7021, "si7021Addr|i2c|Address (0 - find it)|0") {
    if (i2c_bus()) {
        auto* sensor = new SI7021Sensor();
        sensor->setAddress(getSetting("si7021Addr", uint8_t{ SI7021_ADDRESS }));
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
