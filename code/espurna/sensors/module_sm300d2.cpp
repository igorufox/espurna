/*

SENSOR DRIVER: SM300D2 air quality (UART)
A module of its own (sensors/driver.h): needs the sensors module and the uart module

*/

#if MODULE_BUILD

#include "driver.h"
#include "SM300D2Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(sm300d2, "sm300d2Port|uart|UART port|0") {
    if (auto port = uart_port(F("sm300d2Port"))) {
        auto* sensor = new SM300D2Sensor();
        sensor->setPort(port->stream);
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
