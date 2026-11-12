/*

The curtain_kingart module (docs/modules.md): the KingArt curtain motor on a UART port, needs the uart module.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "curtain_kingart.h"

ESPURNA_MODULE(curtain_kingart, "curtain_kingart", 50) {
    curtainSetup();
}

#endif // MODULE_BUILD
