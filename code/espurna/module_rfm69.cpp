/*

The rfm69 module (docs/modules.md): the RFM69 radio on SPI (LowPowerLab RFM69): messages of the nodes to MQTT.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "rfm69.h"

ESPURNA_MODULE(rfm69, "rfm69", 50) {
    rfm69Setup();
}

#endif // MODULE_BUILD
