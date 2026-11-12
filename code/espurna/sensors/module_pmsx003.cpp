/*

SENSOR DRIVER: PMSx003 / PMS5003 particles (UART)
A module of its own (sensors/driver.h): needs the sensors module and the uart module

*/

#if MODULE_BUILD

#include "driver.h"
#include "PMSX003Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(pmsx003, "pmsPort|uart|UART port|0;pmsType|select|Type|0|0=PMSx003,1=PMSx003 9 values,2=PMS5003T,3=PMS5003ST,4=PMS5003S") {
    if (auto port = uart_port(F("pmsPort"))) {
        auto* sensor = new PMSX003Sensor();
        sensor->setPort(port->stream);
        sensor->setType(getSetting(F("pmsType"), uint8_t{ PMS_TYPE }));
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
