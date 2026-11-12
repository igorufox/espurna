/*

SENSOR DRIVER: PM1006 particles (UART)
A module of its own (sensors/driver.h): needs the sensors module and the uart module

*/

#if MODULE_BUILD

#include "driver.h"
#include "PM1006Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(pm1006, "pm1006Port|uart|UART port|0") {
    if (auto port = uart_port(F("pm1006Port"))) {
        auto* sensor = new PM1006Sensor();
        sensor->setPort(port->stream);
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
