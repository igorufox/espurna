/*

SENSOR DRIVER: Event counters (interrupts)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "EventSensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(events, "eventsPin#|gpio|GPIO #|||8;eventsPullUp#|bool|Pull-up #|||8;eventsIntMode#|select|Interrupt #||1=Rising,2=Falling,3=Change|8;eventsDebounce#|number|Debounce # (ms)||0-60000|8") {
    for (size_t index = 0; index < EventSensor::SensorsMax; ++index) {
        const auto pin = getSetting({"eventsPin", index}, EventSensor::defaultPin(index));
        if (!pin_free(pin)) {
            break;
        }

        auto mode = EventSensor::defaultPinMode(index);
        const auto pullup = getSetting({"eventsPullUp", index}, mode == espurna::pins::InputPullup);
        if (pullup != (mode == espurna::pins::InputPullup)) {
            mode = pullup ? espurna::pins::InputPullup : espurna::pins::Input;
        }

        auto* sensor = new EventSensor();
        sensor->setPin(pin);
        sensor->setPinMode(mode);
        sensor->setDebounceTime(
            espurna::duration::Milliseconds(getSetting({"eventsDebounce", index},
                static_cast<uint32_t>(EventSensor::defaultDebounceTime(index).count()))));
        sensor->setInterruptMode(
            getSetting({"eventsIntMode", index},
                static_cast<uint8_t>(EventSensor::defaultInterruptMode(index))));
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
