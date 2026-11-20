/*

The ble module (docs/modules.md): the Bluetooth LE of the ESP32 and the BK7231N, the stack of the chip and the
passive scan (ble.cpp, ble.h, arch/<chip>/ble_<chip>.cpp). The bus of the BLE modules (ble_sensors ...): they need
it and are linked against it directly. No ESP8266 (no Bluetooth there: custom_modules_skip).

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "ble.h"

ESPURNA_MODULE(ble, "ble", 9) {
    bleSetup();
}

#endif // MODULE_BUILD
