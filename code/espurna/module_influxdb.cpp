/*

The influxdb module (docs/modules.md): sends the relays and the sensors to InfluxDB (the head's HTTP client).

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "influxdb.h"

ESPURNA_MODULE(influxdb, "influxdb", 50) {
    idbSetup();
}

#endif // MODULE_BUILD
