/*

SENSOR DRIVER: PZEM-004T v1/v2 power meters (UART)
A module of its own (sensors/driver.h): needs the sensors module and the uart module

*/

#if MODULE_BUILD

#include "driver.h"
#include "PZEM004TSensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(pzem004t, "pzemPort|uart|UART port|0;pzemAddr#|text|Address of meter # (192.168.1.x)|||4") {
    if (auto port = uart_port("pzemPort")) {
        auto serial = std::make_shared<PZEM004TSensor::SerialPort>(port->stream);

        bool initialized { false };
        for (size_t index = 0; index < PZEM004TSensor::DevicesMax; ++index) {
            auto address = getSetting({"pzemAddr", index}, PZEM004TSensor::defaultAddress(index));
            if (!espurna::chip::address_set(address)) {
                break;
            }

            auto* ptr = PZEM004TSensor::make(serial, address);
            if (ptr) {
                sensorAdd(ptr);
                initialized = true;
            }
        }

        if (initialized) {
            PZEM004TSensor::registerTerminalCommands();
        }
    }
}

#endif // MODULE_BUILD
