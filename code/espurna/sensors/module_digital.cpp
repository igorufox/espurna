/*

SENSOR DRIVER: Digital inputs
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "DigitalSensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(digital, "digitalPin#|gpio|GPIO #|||8;digitalPinMode#|select|Mode #||default=Input,pull-up=Pull-up,pull-down=Pull-down|8;digitalDefState#|select|State at boot #||low=Low,high=High,initial=As read|8") {
    if (pin_set(espurna::sensor::driver::digital::settings::pin(0))) {
        espurna::sensor::driver::digital::load();
    }
}

#endif // MODULE_BUILD
