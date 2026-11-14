/*

The rfm69 module (docs/modules.md): the RFM69 radio on SPI (LowPowerLab RFM69): messages of the nodes to MQTT.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "rfm69.h"

#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_rfm69.ipp"
#endif

ESPURNA_MODULE(rfm69, "rfm69", 50) {
#if WEB_SUPPORT
    modulesUi("rfm69", ui_rfm69_data, sizeof(ui_rfm69_data));
#endif

    rfm69Setup();
}

#endif // MODULE_BUILD
