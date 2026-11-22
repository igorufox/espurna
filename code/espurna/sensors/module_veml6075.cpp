/*

SENSOR DRIVER: VEML6075 UV (I2C)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "VEML6075Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(veml6075, "veml6075Time|select|Integration time|1|0=50 ms,1=100 ms,2=200 ms,3=400 ms,4=800 ms;veml6075High|bool|High dynamic mode|0") {
    if (i2c_bus()) {
        auto* sensor = new VEML6075Sensor();
        sensor->setIntegrationTime(getSetting("veml6075Time", static_cast<int>(VEML6075_INTEGRATION_TIME)));
        sensor->setDynamicMode(getSetting("veml6075High", VEML6075_DYNAMIC_MODE != 0));
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
