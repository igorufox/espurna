/*

The i2c module (docs/modules.md): the I2C bus of the board, i2c.cpp on the chip's I2C master (i2c_* of
arch/rtos/chip_sdk.h, no Arduino layer; settings i2cSDA / i2cSCL, the bus starts when both are set). The I2C sensor
drivers need it and are linked against it directly; the libraries of a few (VL53L1X, VEML6075, SI1145, BSEC) talk
through Wire of the arduino_emu module, on the same chip bus: the head has no I2C (owner 2026-11-12).

The SENSORS panel shows its pins above the drivers, in the spec format of the drivers (espurna::sensor::Driver of
sensor.h): `snsBus`.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "i2c.h"
#include "ws.h"

namespace {

#if WEB_SUPPORT
alignas(4) static constexpr char Spec[] PROGMEM = "i2cSDA|gpio|SDA|;i2cSCL|gpio|SCL|";

void onConnected(espurna::json::Writer& root) {
    root.array(STRING_VIEW("snsBus"));
    root.beginObject();
    root.field(STRING_VIEW("name"), STRING_VIEW("i2c"));
    root.field(STRING_VIEW("spec"), Spec);

    root.object(STRING_VIEW("values"));
    root.field(STRING_VIEW("i2cSDA"), getSetting("i2cSDA"));
    root.field(STRING_VIEW("i2cSCL"), getSetting("i2cSCL"));
    root.end();
    root.end();
    root.end();
}

bool onKeyCheck(espurna::StringView key, espurna::json::Value) {
    return key.startsWith(STRING_VIEW("i2c"));
}
#endif

} // namespace

ESPURNA_MODULE(i2c, "i2c", 9) {
    i2cSetup();
#if WEB_SUPPORT
    wsRegister()
        .onConnected(onConnected)
        .onKeyCheck(onKeyCheck);
#endif
}

#endif // MODULE_BUILD
