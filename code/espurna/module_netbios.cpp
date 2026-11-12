/*

The netbios module (docs/modules.md): NetBIOS name service: the hostname of the board for Windows, the same responder on every chip (lwIP).

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "netbios.h"

ESPURNA_MODULE(netbios, "netbios", 50) {
    netbiosSetup();
}

#endif // MODULE_BUILD
