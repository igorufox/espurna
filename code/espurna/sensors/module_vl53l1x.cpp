/*

SENSOR DRIVER: VL53L1X laser distance (I2C)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "VL53L1XSensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(vl53l1x, "vl53l1xMode|select|Distance mode|2|0=Short,1=Medium,2=Long;vl53l1xBudget|number|Timing budget (us)|140000|20000-1000000;vl53l1xPeriod|number|Period (ms)|50|1-10000") {
    if (i2c_bus()) {
        auto* sensor = new VL53L1XSensor();
        sensor->setInterMeasurementPeriod(VL53L1XSensor::InterMeasurementPeriod{
            getSetting(F("vl53l1xPeriod"), static_cast<uint32_t>(VL53L1X_INTER_MEASUREMENT_PERIOD))});
        sensor->setMeasurementTimingBudget(VL53L1XSensor::MeasurementTimingBudget{
            getSetting(F("vl53l1xBudget"), static_cast<uint32_t>(VL53L1X_MEASUREMENT_TIMING_BUDGET))});
        sensor->setDistanceMode(static_cast<VL53L1X::DistanceMode>(
            getSetting(F("vl53l1xMode"), static_cast<int>(VL53L1X_DISTANCE_MODE))));
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
