/*

NEXTION MODULE

A Nextion display on a UART port (the NSPanel's: OHpanel or Lovelace UI TFT, detected automatically): the display of
the tile pages of the screen module (screen_pages.h) and a panel of the screen with pages of its own (the screen draws
on it only its QR page, nextion_screen.cpp). The parts: screen_pages.h. docs/modules.md, the `nextion` module.

*/

#pragma once

#include <cstdint>

// Early, right after the basic modules (works in the 'unstable' system mode as well): the display power pin
void nextionBootSetup();

// the UART port, the display detection, the TFT upload, the panel of the screen
void nextionSetup();

// the Nextion as the panel of the screen (nextion_screen.cpp)
void nextionScreenSetup();

// The blank page of the OHpanel driver for the screen's QR page (nextion_screen.cpp): it loads it and calls
// draw(width, height), which sends nextionScreenFill() / nextionScreenText(): on the show and on every redraw (TFT
// restart, the page put back, the WebUI mirror). A touch, or another page asked for (NSPANEL.PAGE), ends it: closed(),
// then the page before. nextionScreenShow(): 0 shown, 1 no display connected, 2 the active driver has no such screen
// (Lovelace)
using NextionScreenDraw = void (*)(int width, int height);
using NextionScreenClosed = void (*)();

int nextionScreenShow(NextionScreenDraw draw, NextionScreenClosed closed);

// back to the page before, without closed()
void nextionScreenHide();

// the screen is on the display now
bool nextionScreenShown();

// only from draw(): a filled rectangle; a line of text in the box (fonts of the TFT: 0 16 px .. 3 26 px; centre 0
// left, 1 centre, 2 right)
void nextionScreenFill(int x, int y, int w, int h, uint16_t colour);
void nextionScreenText(int x, int y, int w, int h, int font, uint16_t colour, uint16_t back, int centre,
    const char* text);
