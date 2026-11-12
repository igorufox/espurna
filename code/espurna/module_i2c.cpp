/*

The i2c module (docs/modules.md): the I2C bus of the board, i2c.cpp and the one Wire (settings i2cSDA / i2cSCL, the
bus starts when both are set; the BK7231N has the software Wire of lib_beken/Wire). The I2C sensor drivers need it
and are linked against it directly, their libraries (VL53L1X, VEML6075, SI1145, BSEC) take its Wire: the head has no
I2C (owner 2026-11-12).

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

void onConnected(JsonObject& root) {
    JsonArray& out = root.createNestedArray(F("snsBus"));
    JsonObject& entry = out.createNestedObject();
    entry[F("name")] = F("i2c");
    entry[F("spec")] = FPSTR(Spec);

    JsonObject& values = entry.createNestedObject(F("values"));
    values[F("i2cSDA")] = getSetting(F("i2cSDA"));
    values[F("i2cSCL")] = getSetting(F("i2cSCL"));
}

bool onKeyCheck(espurna::StringView key, const JsonVariant&) {
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
