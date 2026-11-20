/*

SENSOR DRIVER: SDS011 particles (UART)
A module of its own (sensors/driver.h): needs the sensors module and the uart module

*/

#if MODULE_BUILD

#include "driver.h"
#include "SDS011Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(sds011, "sds011Port|uart|UART port|0") {
    if (auto port = uart_port("sds011Port")) {
        auto* sensor = new SDS011Sensor();
        sensor->setPort(port->stream);
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
