/*

SENSOR DRIVER: SenseAir S8 CO2 (UART)
A module of its own (sensors/driver.h): needs the sensors module and the uart module

*/

#if MODULE_BUILD

#include "driver.h"
#include "SenseAirSensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(senseair, "senseairPort|uart|UART port|0") {
    if (auto port = uart_port(F("senseairPort"))) {
        auto* sensor = new SenseAirSensor();
        sensor->setPort(port->stream);
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
