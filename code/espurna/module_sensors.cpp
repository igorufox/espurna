/*

PROTOTYPE: the sensors module (docs/modules.md). Compiled in env:esp8266-1m-modules of
platformio_esp8266.ini; scripts/esp8266_module_link.py links it with sensor.cpp against the head.

Provides the magnitude API to the other modules (Home Assistant, Domoticz, Thingspeak ...) through the head.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"

#if I2C_SUPPORT
#include "i2c.h"
#endif

#if ONE_WIRE_SUPPORT
#include "driver_onewire.h"
#endif

#if UART_SUPPORT
#include "uart.h"
#endif

#include "sensor.h"

ESPURNA_MODULE(sensors, "sensors", 10) {
    using espurna::modules::Api;

#if UART_SUPPORT
    if (ESPURNA_MODULE_OWNS(sensors, uartSetup)) {
        uartSetup();
    }
#endif
#if I2C_SUPPORT
    if (ESPURNA_MODULE_OWNS(sensors, i2cSetup)) {
        i2cSetup();
    }
#endif
#if ONE_WIRE_SUPPORT
    if (ESPURNA_MODULE_OWNS(sensors, oneWireSetup)) {
        oneWireSetup();
    }
#endif

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
