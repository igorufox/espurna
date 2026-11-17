/*

The nspanelqr module (docs/modules.md): the Matter pairing QR code on the NSPanel display (nspanelqr.cpp); needs the
nspanel and the matter module, linked against both directly.

*/

#include "espurna.h"

#if MODULE_BUILD && NSPANEL_SUPPORT && MATTER_SUPPORT

#include "modules.h"
#include "nspanel.h"

// after nspanel and matter (60)
ESPURNA_MODULE(nspanelqr, "nspanelqr", 70) {
    nspanelQrSetup();
}

#endif
