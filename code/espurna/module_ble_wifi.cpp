/*

The ble_wifi module (docs/modules.md): Wi-Fi setup over BLE, Improv Wi-Fi (ble_wifi.cpp). Needs the ble module (its
GATT server) and is linked against it directly. ESP32 and BK7231N only (no Bluetooth on the ESP8266).

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "ble.h"

ESPURNA_MODULE(ble_wifi, "ble_wifi", 12) {
    bleWifiSetup();
}

#endif // MODULE_BUILD
