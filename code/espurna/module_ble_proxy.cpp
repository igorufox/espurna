/*

The ble_proxy module (docs/modules.md): the advertisements the board hears, raw, to MQTT (ble_proxy.cpp). Needs the
ble module (the scanner) and the mqtt module. ESP32 and BK7231N only (no Bluetooth on the ESP8266).

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "ble.h"

ESPURNA_MODULE(ble_proxy, "ble_proxy", 50) {
    bleProxySetup();
}

#endif // MODULE_BUILD
