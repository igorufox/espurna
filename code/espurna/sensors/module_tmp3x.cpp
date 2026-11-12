/*

SENSOR DRIVER: TMP35 / TMP36 temperature (analog)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "AnalogSensor.h"
#include "TMP3XSensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(tmp3x, "tmp3xType|select|Type|35|35=TMP35,36=TMP36") {
    auto* sensor = new TMP3XSensor();
    sensor->setType(getSetting(F("tmp3xType"), uint8_t{ TMP3X_TYPE }));
    sensorAdd(sensor);
}

#endif // MODULE_BUILD
