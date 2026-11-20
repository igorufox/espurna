/*

SENSOR DRIVER: HLW8012 / BL0937 power meter
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "HLW8012Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(hlw8012, "hlw8012SEL|gpio|SEL|;hlw8012CF|gpio|CF|;hlw8012CF1|gpio|CF1|;hlw8012SelCurr|bool|SEL high measures the current|1;hlw8012IntMode|select|Interrupt|3|1=Rising,2=Falling,3=Change") {
    // CF and CF1 are locked when their interrupts are attached (and are only read otherwise).
    // SEL is not locked, ESPurna H06 shares it with its LED
    const auto sel = getSetting("hlw8012SEL", uint8_t{ HLW8012_SEL_PIN });
    const auto cf = getSetting("hlw8012CF", uint8_t{ HLW8012_CF_PIN });
    const auto cf1 = getSetting("hlw8012CF1", uint8_t{ HLW8012_CF1_PIN });
    if (pin_free(cf) && pin_free(cf1) && pin_set(sel)) {
        auto* sensor = new HLW8012Sensor();
        sensor->setSEL(sel);
        sensor->setCF(cf);
        sensor->setCF1(cf1);
        sensor->setSELCurrent(getSetting("hlw8012SelCurr", HLW8012_SEL_CURRENT == true));
        sensor->setInterruptMode(getSetting("hlw8012IntMode", uint8_t{ HLW8012_INTERRUPT_ON }));
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
