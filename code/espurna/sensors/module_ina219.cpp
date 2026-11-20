/*

SENSOR DRIVER: INA219 current and voltage (I2C)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "INA219Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(ina219, "ina219Addr|i2c|Address (0 - find it)|0;ina219Shunt|number|Shunt resistance (Ohm)|0.1;ina219MaxCurrent|number|Max expected current (A)|2") {
    if (i2c_bus()) {
        auto* sensor = new INA219Sensor();
        sensor->setAddress(getSetting("ina219Addr", uint8_t{ INA219_ADDRESS }));
        sensor->setOperatingMode(INA219Sensor::INA219_OPERATING_MODE);
        sensor->setShuntMode(INA219Sensor::INA219_SHUNT_MODE);
        sensor->setBusMode(INA219Sensor::INA219_BUS_MODE);
        sensor->setBusRange(INA219Sensor::INA219_BUS_RANGE);
        sensor->setGain(INA219Sensor::INA219_GAIN);
        sensor->setShuntResistance(getSetting("ina219Shunt", double{ INA219_SHUNT_RESISTANCE }));
        sensor->setMaxExpectedCurrent(getSetting("ina219MaxCurrent", double{ INA219_MAX_EXPECTED_CURRENT }));
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
