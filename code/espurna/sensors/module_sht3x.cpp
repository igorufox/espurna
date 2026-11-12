/*

SENSOR DRIVER: SHT3x temperature and humidity (I2C)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "SHT3XI2CSensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(sht3x, "sht3xAddr|i2c|Address (0 - find it)|0") {
    if (i2c_bus()) {
        auto* sensor = new SHT3XI2CSensor();
        sensor->setAddress(getSetting(F("sht3xAddr"), uint8_t{ SHT3X_I2C_ADDRESS }));
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
