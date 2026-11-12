/*

The llmnr module (docs/modules.md): LLMNR: Windows resolves the hostname of the board, the same responder on every chip (lwIP).

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "llmnr.h"

ESPURNA_MODULE(llmnr, "llmnr", 50) {
    llmnrSetup();
}

#endif // MODULE_BUILD
