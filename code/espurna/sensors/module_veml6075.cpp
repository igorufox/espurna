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
        sensor->setIntegrationTime(static_cast<VEML6075::veml6075_uv_it_t>(
            getSetting(F("veml6075Time"), static_cast<int>(VEML6075_INTEGRATION_TIME))));
        sensor->setDynamicMode(getSetting(F("veml6075High"), false)
            ? VEML6075::DYNAMIC_HIGH : VEML6075_DYNAMIC_MODE);
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
