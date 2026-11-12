/*

SENSOR DRIVER: BH1750 light (I2C)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "BH1750Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(bh1750, "bh1750Addr|i2c|Address (0 - find it)|0;bh1750Mode|select|Mode|0|0=Continuous high resolution,1=Continuous high resolution 2,2=Continuous low resolution,3=One time high resolution,4=One time high resolution 2,5=One time low resolution") {
    if (i2c_bus()) {
        auto* sensor = new BH1750Sensor();
        sensor->setAddress(getSetting(F("bh1750Addr"), uint8_t{ BH1750_ADDRESS }));
        sensor->setAccuracy(BH1750_ACCURACY);
        sensor->setSensitivity(BH1750_SENSITIVITY);
        sensor->setMode(static_cast<BH1750Sensor::Mode>(
            getSetting(F("bh1750Mode"), static_cast<int>(BH1750_MODE))));
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
