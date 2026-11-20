/*

SENSOR DRIVER: Current clamp on ADC121 (I2C)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "EmonADC121Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(emon_adc121, "emonAdc121Addr|i2c|Address (0 - find it)|0") {
    if (i2c_bus()) {
        auto* sensor = new EmonADC121Sensor();
        sensor->setAddress(getSetting("emonAdc121Addr", uint8_t{ EMON_ADC121_I2C_ADDRESS }));
        sensor->setVoltage(EMON_MAINS_VOLTAGE);
        sensor->setReferenceVoltage(EMON_REFERENCE_VOLTAGE);
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
