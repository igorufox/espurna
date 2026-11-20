/*

SENSOR DRIVER: BME680 air quality (I2C, Bosch BSEC)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "BME680Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(bme680, "bme680Addr|i2c|Address (0 - find it)|0") {
    if (i2c_bus()) {
        auto* sensor = new BME680Sensor();
        sensor->setAddress(getSetting("bme680Addr", uint8_t{ BME680_I2C_ADDRESS }));
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
