/*

The uartmqtt module (docs/modules.md): a UART port bridged to MQTT, needs the uart and mqtt modules.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "uartmqtt.h"

ESPURNA_MODULE(uartmqtt, "uartmqtt", 50) {
    uartMqttSetup();
}

#endif // MODULE_BUILD
