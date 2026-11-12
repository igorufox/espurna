/*

SENSOR DRIVER: MAX6675 thermocouple (SPI)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "MAX6675Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(max6675, "max6675CS|gpio|CS|;max6675SO|gpio|SO|;max6675SCK|gpio|SCK|") {
    const auto cs = getSetting(F("max6675CS"), uint8_t{ GPIO_NONE });
    const auto so = getSetting(F("max6675SO"), uint8_t{ GPIO_NONE });
    const auto sck = getSetting(F("max6675SCK"), uint8_t{ GPIO_NONE });
    if (pin_free(cs) && pin_free(so) && pin_free(sck)) {
        auto* sensor = new MAX6675Sensor();
        sensor->setCS(cs);
        sensor->setSO(so);
        sensor->setSCK(sck);
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
