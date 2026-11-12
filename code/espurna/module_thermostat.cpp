/*

The thermostat module (docs/modules.md): a thermostat on a relay and a temperature of the sensors, with an SSD1306 display on the I2C bus; needs the relay, sensors and i2c modules.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "thermostat.h"

ESPURNA_MODULE(thermostat, "thermostat", 50) {
    thermostatSetup();
    displaySetup();
}

#endif // MODULE_BUILD
