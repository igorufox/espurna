/*

OLED MODULE

The small I2C screen of a board (SSD1306 / SH1106: Heltec WiFi Kit 32, Wemos OLED shield, the thermostat display):
lines of a label and a source, set in the settings, pages of them in turn. docs/modules.md, the `oled` module.

*/

#pragma once

// the button action "display-on" (button.cpp), the head forwards it (modules_api.cpp): wakes the screen up
void displayOn();

void oledSetup();
