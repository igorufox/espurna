/*

NSPANEL MODULE

Sonoff NSPanel (gen1, ESP32) display backend for the OHpanel TFT (nspanel_ohpanel.cpp) and the
joBr99 "NSPanel Lovelace UI" TFT (nspanel_lovelace.cpp), detected automatically (the parts: nspanel_core.h),
(flash layout: esp32_4mb.csv, as on every ESP32 image)

The screen of another part (nspanelScreen* below, OHpanel only for now): nspanelqr.cpp (this module; the matter
module through the head) shows the Matter pairing QR code on it, drawn with Nextion `fill`
rectangles (a horizontal run of dark modules is one fill, the same run in the rows below grows it down) on a white
screen with the quiet zone, the modules as large as the screen takes, one text line under it. It shows while the
pairing window is open and goes when it closes (also when a controller added the device), or on NSPANEL.QR /
NSPANEL.QR off; a touch puts the page back

*/

#pragma once

#include <cstdint>

// Early, right after the basic modules (works in the 'unstable' system mode as well):
// display power and buzzer pins
void nspanelBootSetup();

// Display backend, MQTT, web endpoints, weather and temperature
void nspanelSetup();

// The screen of another module, for the modules that need nspanel (linked directly, espurna/modules_provided.txt).
// The OHpanel driver loads its blank page and calls draw(width, height), which sends nspanelScreenFill() /
// nspanelScreenText(): on the show and on every redraw (TFT restart, the page put back, the WebUI mirror). Nothing of
// the screen stays in the driver. A touch, or another page asked for (NSPANEL.PAGE), ends it: closed(), then the page
// before. nspanelScreenShow(): 0 shown, 1 no display connected, 2 the active driver has no such screen (Lovelace)
using NspanelScreenDraw = void (*)(int width, int height);
using NspanelScreenClosed = void (*)();

int nspanelScreenShow(NspanelScreenDraw draw, NspanelScreenClosed closed);

// back to the page before, without closed()
void nspanelScreenHide();

// the screen is on the display now
bool nspanelScreenShown();

// only from draw(): a filled rectangle; a line of text centred in the box (fonts of the TFT: 0 16 px .. 3 26 px)
void nspanelScreenFill(int x, int y, int w, int h, uint16_t colour);
void nspanelScreenText(int x, int y, int w, int h, int font, uint16_t colour, uint16_t back, const char* text);

// An action of another module on a tile: a "button" item of /nspanel.json with "action": "<name>" (not toggle / on /
// off) calls it on a tap, from the loop of the display driver, both drivers. 'name' is kept as given (a string
// literal): [a-z0-9-_.], at most 24 characters. Registered again: replaced. A tile whose action nobody registered shows
// and does nothing, logged once. nspanelqr.cpp: "matter-pair"
using NspanelAction = void (*)();

void nspanelAction(const char* name, NspanelAction callback);

// the Matter pairing QR code (nspanelqr.cpp)
void nspanelQrSetup();
