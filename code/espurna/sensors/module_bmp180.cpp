/*

SENSOR DRIVER: BMP180 pressure and temperature (I2C)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "BMP180Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(bmp180, "bmp180Addr|i2c|Address (0 - find it)|0") {
    if (i2c_bus()) {
        auto* sensor = new BMP180Sensor();
        sensor->setAddress(getSetting("bmp180Addr", uint8_t{ BMP180_ADDRESS }));
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
