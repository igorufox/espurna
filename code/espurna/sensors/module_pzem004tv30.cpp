/*

SENSOR DRIVER: PZEM-004T v3 power meter (UART)
A module of its own (sensors/driver.h): needs the sensors module and the uart module

*/

#if MODULE_BUILD

#include "driver.h"
#include "PZEM004TV30Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(pzem004tv30, "pzemv30Port|uart|UART port|0;pzemv30Addr|number|Address|248|1-248;pzemv30ReadTimeout|number|Read timeout (ms)|200|10-5000") {
    if (auto port = uart_port("pzemv30Port")) {
        auto* sensor = PZEM004TV30Sensor::make(port->stream,
            getSetting("pzemv30Addr", PZEM004TV30Sensor::DefaultAddress),
            getSetting("pzemv30ReadTimeout", PZEM004TV30Sensor::DefaultReadTimeout));
        sensor->setDebug(getSetting("pzemv30Debug", PZEM004TV30Sensor::DefaultDebug));
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
