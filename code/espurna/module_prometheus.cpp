/*

The prometheus module (docs/modules.md): the /api/metrics endpoint for Prometheus, needs the api module (its key).

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "prometheus.h"

ESPURNA_MODULE(prometheus, "prometheus", 50) {
    prometheusSetup();
}

#endif // MODULE_BUILD
