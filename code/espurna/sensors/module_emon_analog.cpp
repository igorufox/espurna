/*

SENSOR DRIVER: Current clamp on the ADC
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "EmonAnalogSensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(emon_analog, "emonAnalogRes|select|ADC resolution|10|10=10 bits,12=12 bits") {
    auto* sensor = new EmonAnalogSensor();
    sensor->setVoltage(EMON_MAINS_VOLTAGE);
    sensor->setReferenceVoltage(EMON_REFERENCE_VOLTAGE);
    sensor->setResolution(getSetting(F("emonAnalogRes"), EMON_ANALOG_RESOLUTION));
    sensorAdd(sensor);
}

#endif // MODULE_BUILD
