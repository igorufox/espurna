/*

SENSOR DRIVER: Dallas DS18B20 ... temperature (1-Wire)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "../driver_onewire.h"
#include "DallasSensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(dallas, "dallasPin|gpio|GPIO|;dallasParasite|bool|Parasite power|1;dallasResolution|select|Resolution|0|0=As the sensor has it,9=9 bits,10=10 bits,11=11 bits,12=12 bits") {
    oneWireSetup();

    if (pin_set(espurna::sensor::driver::dallas::settings::pin())) {
        espurna::sensor::driver::dallas::load();
    }
}

#endif // MODULE_BUILD
