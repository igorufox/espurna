/*

SENSOR DRIVER: EZO pH (UART)
A module of its own (sensors/driver.h): needs the sensors module and the uart module

*/

#if MODULE_BUILD

#include "driver.h"
#include "EZOPHSensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(ezoph, "ezophPort|uart|UART port|0") {
    if (auto port = uart_port("ezophPort")) {
        auto* sensor = new EZOPHSensor();
        sensor->setPort(port->stream);
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
