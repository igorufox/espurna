/*

SENSOR DRIVER: A02YYU ultrasonic distance (UART)
A module of its own (sensors/driver.h): needs the sensors module and the uart module

*/

#if MODULE_BUILD

#include "driver.h"
#include "A02YYUSensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(a02yyu, "a02yyuPort|uart|UART port|0") {
    if (auto port = uart_port("a02yyuPort")) {
        auto* sensor = new A02YYUSensor();
        sensor->setPort(port->stream);
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
