/*

The screen module (docs/modules.md): what every screen has in common (screen.cpp: the drawing, the fonts and icons,
the pages of lines, the Matter QR page, blanking, the keys, the WebUI mirror). The panels are the modules that need it:
oled (I2C), lcd (SPI), nspanel (the NSPanel's Nextion). It gives the button action "display-on" to the head.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "screen.h"

#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_screen.ipp"
#endif

ESPURNA_MODULE(screen, "screen", 45) {
#if WEB_SUPPORT
    modulesUi("screen", ui_screen_data, sizeof(ui_screen_data));
#endif

    // the button action "display-on"
    modulesProvide(espurna::modules::Api::DisplayOn, &displayOn);

    screenSetup();
}

#endif // MODULE_BUILD
