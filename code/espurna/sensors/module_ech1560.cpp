/*

SENSOR DRIVER: ECH1560 power meter
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "ECH1560Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(ech1560, "ech1560CLK|gpio|CLK|;ech1560MISO|gpio|MISO|;ech1560Inv|bool|Inverted|0") {
    // CLK is locked when its interrupt is attached, MISO is only read and is locked here
    const auto clk = getSetting("ech1560CLK", uint8_t{ ECH1560_CLK_PIN });
    const auto miso = getSetting("ech1560MISO", uint8_t{ ECH1560_MISO_PIN });
    if (pin_free(clk) && pin_free(miso) && gpioLock(miso)) {
        auto* sensor = new ECH1560Sensor();
        sensor->setCLK(clk);
        sensor->setMISO(miso);
        sensor->setInverted(getSetting("ech1560Inv", ECH1560_INVERTED == 1));
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
