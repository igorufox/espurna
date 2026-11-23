/*

The lcd module (docs/modules.md): a colour TFT on the SPI bus of the spi module, ILI9341 / ILI9342 / ST7789 (lcd.cpp: the
controller and its settings), a panel of the screen module, which draws on it (the M5Stack: the nspanel module's pages).

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "lcd.h"

#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_lcd.ipp"
#endif

ESPURNA_MODULE(lcd, "lcd", 50) {
#if WEB_SUPPORT
    modulesUi("lcd", ui_lcd_data, sizeof(ui_lcd_data));
#endif

    lcdSetup();
}

#endif // MODULE_BUILD
