/*

SENSOR DRIVER MODULES - what every sensors/module_<name>.cpp has in common (docs/modules.md)

Every sensor driver is a module of its own (config/head.h: <name> | sensors/module_<name>.cpp.o | | | sensors ...),
started after the sensors module (the manager, sensor.cpp). Its entry registers it (sensorDriver()): the name, the
settings for the SENSORS panel of the WebUI (the spec, ref. espurna::sensor::Driver in sensor.h) and load(). load()
reads the settings and adds the sensors they configure (sensorAdd()); nothing when the pins / the port / the address
are not set, so an installed driver stays idle until its settings say where the sensor is. Pins and ports default to
'none' / 0: a module installed by mistake never takes a GPIO of a relay or the serial port.

The buses: the I2C bus is the i2c module's (i2c.cpp, settings i2cSDA / i2cSCL: one Wire for the drivers and the
libraries of the drivers; an I2C driver needs the i2c module and is linked against it), the UART ports the uart
module's (uartPort / uartClaim through the head), the 1-Wire bus the dallas module's own.

    ESPURNA_SENSOR_DRIVER(dht, "dhtPin|gpio|GPIO||;dhtType|select|Type|22|11=DHT11,22=DHT22") {
        ... read the settings, sensorAdd(new DHTSensor()) ...
    }

*/

#pragma once

#include "../espurna.h"
#include "../espurna_chip.h"
#include "../gpio.h"
#include "../modules.h"
#include "../sensor.h"
#include "../terminal.h"

#include <cfloat>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

// what the driver headers take for granted (sensor.cpp included them before every driver)
#include "../libs/fs_math.h"
#include "BaseSensor.h"
#include "BaseEmonSensor.h"
#include "BaseAnalogEmonSensor.h"
#include "BaseAnalogSensor.h"

#if UART_SUPPORT
#include "../uart.h"
#endif

#if I2C_SUPPORT
#include "../i2c.h"
#endif

namespace espurna {
namespace sensor {
namespace module {

[[gnu::unused]]
inline bool pin_set(uint8_t pin) {
    return pin != GPIO_NONE;
}

// set, valid, and not owned by a relay, button, LED ... that were set up before the sensors
[[gnu::unused]]
inline bool pin_free(uint8_t pin) {
    if (pin_set(pin)) {
        if (gpioValid(pin) && !gpioLocked(pin)) {
            return true;
        }

        DEBUG_MSG_P(PSTR("[SENSOR] GPIO%hhu is invalid or used\n"), pin);
    }

    return false;
}

#if UART_SUPPORT
// <driver>Port: 1-based number of the port set up by uartTx<n-1> / uartRx<n-1> / uartBaud<n-1>, 0 - off. The sensor
// claims the port, the serial log and the serial terminal stop using it; a second sensor on a claimed port is not
// started
[[gnu::unused]]
inline espurna::driver::uart::PortPtr uart_port(const char* key, uint8_t number = 0) {
    number = getSetting(key, number);
    if (!number) {
        return nullptr;
    }

    auto port = uartPort(number - 1);
    if (!port) {
        DEBUG_MSG_P(PSTR("[SENSOR] UART%hhu is not configured\n"), number);
        return port;
    }

    if (!uartClaim(number - 1, key)) {
        return nullptr;
    }

    return port;
}
#endif

#if I2C_SUPPORT
// i2c.cpp only starts the bus when both pins are set
[[gnu::unused]]
inline bool i2c_bus() {
    const bool out = pin_set(getSetting("i2cSDA", uint8_t{ I2C_SDA_PIN }))
        && pin_set(getSetting("i2cSCL", uint8_t{ I2C_SCL_PIN }));
    if (!out) {
        DEBUG_MSG_P(PSTR("[SENSOR] I2C bus is not configured (i2cSDA, i2cSCL)\n"));
    }

    return out;
}
#endif

} // namespace module
} // namespace sensor
} // namespace espurna

// The module of one driver: its spec (PROGMEM, the settings for the WebUI) and the body of its load()
#define ESPURNA_SENSOR_DRIVER(ID, SPEC) \
    static void espurna_sensor_load_##ID(); \
    alignas(4) static constexpr char espurna_sensor_spec_##ID[] PROGMEM = SPEC; \
    ESPURNA_MODULE(ID, #ID, 11) { \
        sensorDriver(espurna::sensor::Driver{ #ID, espurna_sensor_spec_##ID, &espurna_sensor_load_##ID }); \
    } \
    static void espurna_sensor_load_##ID()
