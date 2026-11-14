/*

The influxdb module (docs/modules.md): sends the relays and the sensors to InfluxDB (the head's HTTP client).

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "influxdb.h"

#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_influxdb.ipp"
#endif

ESPURNA_MODULE(influxdb, "influxdb", 50) {
#if WEB_SUPPORT
    modulesUi("influxdb", ui_influxdb_data, sizeof(ui_influxdb_data));
#endif

    idbSetup();
}

#endif // MODULE_BUILD
