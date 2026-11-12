/*

The rpnrules module (docs/modules.md): RPN rules (RPNlib): small programs on the relays, the sensors, MQTT and the time; before the relays (it follows their changes from the start).

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "rpnrules.h"

ESPURNA_MODULE(rpnrules, "rpnrules", 11) {
    rpnSetup();
}

#endif // MODULE_BUILD
