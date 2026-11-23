/*

The oled module (docs/modules.md): the small I2C screen of a board, SSD1306 or SH1106 (oled.cpp: the controller and
its settings), a panel of the screen module, which draws on it; the bus is the one of the i2c module.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "oled.h"

#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_oled.ipp"
#endif

ESPURNA_MODULE(oled, "oled", 50) {
#if WEB_SUPPORT
    modulesUi("oled", ui_oled_data, sizeof(ui_oled_data));
#endif

    oledSetup();
}

#endif // MODULE_BUILD
