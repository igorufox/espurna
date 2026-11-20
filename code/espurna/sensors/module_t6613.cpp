/*

SENSOR DRIVER: T6613 CO2 (UART)
A module of its own (sensors/driver.h): needs the sensors module and the uart module

*/

#if MODULE_BUILD

#include "driver.h"
#include "T6613Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(t6613, "t6613Port|uart|UART port|0") {
    if (auto port = uart_port("t6613Port")) {
        auto* sensor = new T6613Sensor();
        sensor->setPort(port->stream);
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
