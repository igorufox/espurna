/*

SENSOR DRIVER: Ultrasonic distance HC-SR04 ...
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "SonarSensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(sonar, "sonarTrigger|gpio|Trigger GPIO|;sonarEcho|gpio|Echo GPIO|;sonarMaxDistance|number|Max distance (cm)|500|1-500;sonarIterations|number|Pings per reading|5|1-20") {
    const auto trigger = getSetting(F("sonarTrigger"), uint8_t{ GPIO_NONE });
    const auto echo = getSetting(F("sonarEcho"), uint8_t{ GPIO_NONE });
    if (pin_free(trigger) && pin_free(echo)) {
        auto* sensor = new SonarSensor();
        sensor->setTrigger(trigger);
        sensor->setEcho(echo);
        sensor->setMaxDistance(getSetting(F("sonarMaxDistance"), static_cast<unsigned int>(SONAR_MAX_DISTANCE)));
        sensor->setIterations(getSetting(F("sonarIterations"), static_cast<unsigned int>(SONAR_ITERATIONS)));
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
