/*

SENSOR DRIVER: AM2320 temperature and humidity (I2C)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "AM2320Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(am2320, "am2320Addr|i2c|Address (0 - find it)|0") {
    if (i2c_bus()) {
        auto* sensor = new AM2320Sensor();
        sensor->setAddress(getSetting(F("am2320Addr"), uint8_t{ AM2320_ADDRESS }));
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
