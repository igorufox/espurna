/*

The encoder module (docs/modules.md): rotary encoders: the brightness and the channels of the lights, needs the light module.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "encoder.h"

ESPURNA_MODULE(encoder, "encoder", 13) {
    encoderSetup();
}

#endif // MODULE_BUILD
