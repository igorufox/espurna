/*

SENSOR DRIVER: HDC1080 temperature and humidity (I2C)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "HDC1080Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(hdc1080, "hdc1080Addr|i2c|Address (0 - find it)|0") {
    if (i2c_bus()) {
        auto* sensor = new HDC1080Sensor();
        sensor->setAddress(getSetting(F("hdc1080Addr"), uint8_t{ HDC1080_ADDRESS }));
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
