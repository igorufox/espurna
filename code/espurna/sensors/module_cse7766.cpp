/*

SENSOR DRIVER: CSE7766 power meter (UART)
A module of its own (sensors/driver.h): needs the sensors module and the uart module

*/

#if MODULE_BUILD

#include "driver.h"
#include "CSE7766Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(cse7766, "cse7766Port|uart|UART port|0") {
    if (auto port = uart_port("cse7766Port")) {
        auto* sensor = new CSE7766Sensor();
        sensor->setPort(port->stream);
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
