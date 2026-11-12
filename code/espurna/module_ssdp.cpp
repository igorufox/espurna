/*

The ssdp module (docs/modules.md): SSDP: Windows and UPnP see the board in the network (its description.xml), the same responder on every chip (lwIP).

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "ssdp.h"

ESPURNA_MODULE(ssdp, "ssdp", 50) {
    ssdpSetup();
}

#endif // MODULE_BUILD
