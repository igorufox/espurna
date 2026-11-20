/*

The matter_ble module (docs/modules.md): Matter commissioning over BLE (matter_ble.cpp). Needs the ble module (its GATT
server) and the matter module (the Matter library, its BLE transport), linked against both directly. ESP32 and BK7231N
only (no Bluetooth and no Matter on the ESP8266).

*/

#include "espurna.h"

#if MODULE_BUILD && MATTER_SUPPORT

#include "modules.h"

void matterBleSetup();

// after the matter module (60): the library is there when the service attaches
ESPURNA_MODULE(matter_ble, "matter_ble", 61) {
    matterBleSetup();
}

#endif
