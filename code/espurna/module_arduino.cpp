/*

The arduino module (docs/modules.md, regress-todo.md item 30): the layer of the Arduino names (lib/espurna-core: String,
Print, wiring, Wire, SPI, Serial, WiFiUDP) for the third-party libraries of the modules (sensor drivers, IR, RF, OLED,
RPN rules). Owner 18.11: «ардуино сделать как модулем, но на него ссылаться прям только если совсем никак». The head and
the modules of ESPurna's own code do not use it; a module whose library does has arduino in its needs column
(config/head.h) and is linked against it, the layer is on the board once. scripts/module_link.py links the layer into
this module only (LIBRARY_MODULES). Beken: LibreTiny's Arduino core is in the head until the BDK build, the module is
empty there.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"

ESPURNA_MODULE(arduino, "arduino", 1) {
}

#endif // MODULE_BUILD
