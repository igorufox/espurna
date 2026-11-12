/*

SENSOR DRIVER: LDR light (analog)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "LDRSensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(ldr, "ldrPin|adc|Pin|;ldrType|select|Type|2|1=GL5516,2=GL5528,3=GL5537-1,4=GL5537-2,5=GL5539,6=GL5549,99=Other;ldrOnGround|bool|Photocell on the ground side|1;ldrResistor|number|Resistor (Ohm)|10000") {
    const auto pin = getSetting(F("ldrPin"), uint8_t{ LDR_PIN });
    if (pin_set(pin)) {
        auto* sensor = new LDRSensor();
        sensor->setPin(pin);
        sensor->setSamples(LDR_SAMPLES);
        sensor->setDelay(LDR_DELAY);
        sensor->setType(getSetting(F("ldrType"), uint8_t{ LDR_TYPE }));
        sensor->setPhotocellPositionOnGround(getSetting(F("ldrOnGround"), static_cast<bool>(LDR_ON_GROUND)));
        sensor->setResistor(getSetting(F("ldrResistor"), static_cast<unsigned long>(LDR_RESISTOR)));
        sensor->setPhotocellParameters(LDR_MULTIPLICATION, LDR_POWER);
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
