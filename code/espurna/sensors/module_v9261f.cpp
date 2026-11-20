/*

SENSOR DRIVER: V9261F power meter (UART)
A module of its own (sensors/driver.h): needs the sensors module and the uart module

*/

#if MODULE_BUILD

#include "driver.h"
#include "V9261FSensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(v9261f, "v9261fPort|uart|UART port|0") {
    if (auto port = uart_port("v9261fPort")) {
        auto* sensor = new V9261FSensor();
        sensor->setPort(port->stream);
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
