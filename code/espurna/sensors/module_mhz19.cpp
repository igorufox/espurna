/*

SENSOR DRIVER: MH-Z19 CO2 (UART)
A module of its own (sensors/driver.h): needs the sensors module and the uart module

*/

#if MODULE_BUILD

#include "driver.h"
#include "MHZ19Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(mhz19, "mhz19Port|uart|UART port|0;mhz19CalibrateAuto|bool|Automatic calibration|0;mhz19DetectionRange|select|Range (ppm)|2000|2000=2000,5000=5000,10000=10000") {
    if (auto port = uart_port(F("mhz19Port"))) {
        auto* sensor = new MHZ19Sensor();
        sensor->setPort(port->stream);
        sensor->setCalibrateAuto(getSetting(F("mhz19CalibrateAuto"), MHZ19_CALIBRATE_AUTO == 1));
        sensor->setDetectionRange(getSetting(F("mhz19DetectionRange"), uint32_t{ MHZ19_DETECTION_RANGE }));
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
