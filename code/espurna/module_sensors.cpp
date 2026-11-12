/*

The sensors module (docs/modules.md): the sensor manager of sensor.cpp, the magnitudes, their settings, MQTT /
WebSocket / API, the energy. No drivers: every driver is a module of its own (sensors/module_<name>.cpp), it needs this
one and starts after it, registers with sensorDriver() and adds its sensors with sensorAdd(). The I2C bus is the
i2c module's (i2c.cpp: one Wire for the drivers and the libraries of the drivers), the 1-Wire bus the dallas module's.

Provides the magnitude API to the other modules (Home Assistant, Domoticz, Thingspeak ...) through the head
(modules_api.cpp: they work without it too). The driver API (sensorDriver(), sensorAdd() ...) is not in the head: a
driver needs this module and is linked against it directly.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "sensor.h"

ESPURNA_MODULE(sensors, "sensors", 10) {
    using espurna::modules::Api;

    // the real ones (renamed in this module), before anything else calls them
    modulesProvide(Api::MagnitudeCount, &magnitudeCount);
    modulesProvide(Api::MagnitudeInfo, &magnitudeInfo);
    modulesProvide(Api::MagnitudeTypeTopic, &magnitudeTypeTopic);
    modulesProvide(Api::MagnitudeUnitsName, &magnitudeUnitsName);
    modulesProvide(Api::SensorReady, &sensorReady);
    modulesProvide(Api::SensorWebSocketMagnitudes, &sensorWebSocketMagnitudes);
    modulesProvide(Api::SensorOnMagnitudeRead, &sensorOnMagnitudeRead);
    modulesProvide(Api::SensorOnMagnitudeReport, &sensorOnMagnitudeReport);
    modulesProvide(Api::MagnitudeValue, &magnitudeValue);
    modulesProvide(Api::MagnitudeReportValue, &magnitudeReportValue);
    modulesProvide(Api::MagnitudeReadValue, &magnitudeReadValue);
    modulesProvide(Api::MagnitudeType, &magnitudeType);
    modulesProvide(Api::MagnitudeIndex, &magnitudeIndex);
    modulesProvide(Api::MagnitudeTopic, &magnitudeTopic);

    sensorSetup();
}

#endif // MODULE_BUILD
