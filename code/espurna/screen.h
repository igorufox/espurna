/*

SCREEN MODULE

What every screen of a board has in common (docs/modules.md, the `screen` module), one implementation for every panel:

- the drawing: filled boxes, lines, circles, text lines in the fonts of screen_fonts.h (Latin-1 and Cyrillic), the
  icons of screen_icons.h (the tile pages config's, Material Design Icons), RGB565 colours (a 1-bit panel lights what is
  brighter than half)
- the pages of lines (scrLbl<n> / scrSrc<n>: time, date, relay:<id>, sensor:<type>, mqtt:<topic>[#key] ...) when no
  other module draws on the screen
- the Matter pairing QR code while its window is open (or SCREEN.QR), over whatever the screen shows
- blanking after scrOff seconds and the brightness, the button action "display-on" and the keys (SCREEN.KEY: prev,
  next, enter, back - a button with a terminal command action, the WebUI)
- the WebUI mirror of what is drawn (the SCREEN panel)

The panels are the modules that depend on it, each one a driver of its bus:
- oled:    SSD1306 / SH1106 on I2C (oled.cpp), 1 bit
- lcd:     ILI9341 / ILI9342 / ST7789 on SPI (lcd.cpp), RGB565: M5Stack, any SPI TFT soldered to an ESP32 or a Beken
- nextion: a Nextion display (nextion_screen.cpp, the Sonoff NSPanel's): it draws its own pages, the screen draws on it
           only its QR page (the panel calls screenPanelDraw() from its own redraw)

What draws instead of the lines (screenContent()): the tile pages of /nspanel.json (screen_pages_light.cpp) on a panel
without pages of its own (the M5Stack).

*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

// RGB565
constexpr uint16_t ScreenBlack { 0x0000 };
constexpr uint16_t ScreenWhite { 0xffff };

// a background colour, or none: what is under it stays
constexpr int32_t ScreenNoBack { -1 };

constexpr uint16_t screenRgb(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

enum class ScreenFont : uint8_t {
    Small,      // 13 px lines
    Normal,     // 17 px
    Large,      // 23 px
    Big,        // 33 px, digits and the signs of a time / a temperature
    Huge,       // 52 px, the same
};

enum class ScreenAlign : uint8_t {
    Left,
    Center,
    Right,
};

enum class ScreenKey : uint8_t {
    Prev,
    Next,
    Enter,
    Back,
};

// A panel: the driver module gives it to the screen when it answers (screenPanel()), the screen draws through it
class ScreenPanel {
public:
    virtual const char* name() const = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;

    // RGB565, else 1 bit
    virtual bool colour() const = 0;

    // the panel shows pages of its own (a Nextion): the screen takes it only for its QR page (show() /
    // hide()) and draws only when the panel asks for it, from its redraw (screenPanelDraw())
    virtual bool ownPages() const {
        return false;
    }

    virtual bool show() {
        return true;
    }

    virtual void hide() {
    }

    // the page of the screen is still on it (an own-pages panel: not touched away, not replaced)
    virtual bool shown() const {
        return true;
    }

    virtual void fill(int x, int y, int w, int h, uint16_t colour) = 0;

    // 1-bit rows ((w + 7) / 8 bytes a row, the high bit on the left): the set bits in `fore`, the others in `back`
    // (ScreenNoBack: left as they are). The default: a fill of every run of set bits (and of clear ones)
    virtual void bits(int x, int y, int w, int h, const uint8_t* rows, uint16_t fore, int32_t back);

    // a text line in fonts of the panel's own (the Nextion): true when it drew it, else the screen does with its
    // fonts through bits()
    virtual bool text(int, int, int, int, ScreenFont, uint16_t, int32_t, ScreenAlign, const char*) {
        return false;
    }

    // the end of a frame: what changed goes to the panel (oled: the changed pages of 8 rows)
    virtual void flush() {
    }

    virtual void power(bool) {
    }

    // 0..100 %
    virtual void brightness(uint8_t) {
    }

    // what the panel shows, RGB565 rows of the box (GET /screen.raw: the pictures of the documentation); false: the
    // panel cannot be read
    virtual bool read(int, int, int, int, uint16_t*) {
        return false;
    }

protected:
    ~ScreenPanel() = default;
};

// the driver module: its panel answers (nullptr: it is gone). One panel at a time: a second one is refused (false)
bool screenPanel(ScreenPanel* panel);

// the panel lost what it showed (its restart): everything is drawn again
void screenPanelRedraw();

// an own-pages panel: draw the screen's page now (only from the panel's redraw), and: it was touched away
void screenPanelDraw();
void screenPanelClosed();

// What draws on the screen instead of the lines (the tile pages on a pixel panel); nullptr: the lines
struct ScreenContent {
    const char* name;
    // the page in full (after a clear, a wake, the QR page gone, a WebUI mirror) or only what changed since the last
    void (*draw)(bool full);
    void (*key)(ScreenKey key);
};

void screenContent(const ScreenContent* content);

// the local time for what the screen shows: false while the time is not set (NTP not synced yet)
bool screenLocalTime(tm& out);

// the first code point of a UTF-8 text (BMP), 0 for none
uint16_t screenFirstCode(const char* text);

// the content changed: draw(false) at the next loop
void screenInvalidate();

// the panel and what draws on it
bool screenReady();
const char* screenPanelName();
int screenWidth();
int screenHeight();
bool screenColour();
bool screenOn();

// the QR page is on the screen (it carries the Matter setup passcode: nothing prints the pixels then)
bool screenQrShown();

// the panel shows pages of its own (the Nextion of the nextion module)
bool screenOwnPages();

// drawing: from draw() of the content (the screen calls flush after it)
void screenFill(int x, int y, int w, int h, uint16_t colour);
void screenFrame(int x, int y, int w, int h, int thickness, uint16_t colour);
void screenLine(int x0, int y0, int x1, int y1, uint16_t colour);
void screenCircle(int x, int y, int r, uint16_t colour, bool filled);

// a text line in the box (w, h): aligned in it, vertically centred, cut at its right edge. back: the box filled
// first, ScreenNoBack - not. UTF-8, the chars the fonts do not have are left out
void screenText(int x, int y, int w, int h, ScreenFont font, uint16_t fore, int32_t back, ScreenAlign align,
    const char* text);
int screenTextWidth(ScreenFont font, const char* text);
int screenFontHeight(ScreenFont font);

// an icon of the tile pages config (its code point, screen_pages_icons.h) in a square of `size`: 24, 48 or 96 px (96: the
// weather icons, the others are the 48 ones; another size: the nearest smaller one, centred). false: no such icon
bool screenIcon(int x, int y, int size, uint16_t code, uint16_t fore, int32_t back);

// blanking, from the content's own settings (the screensaver of the tile pages config): after `seconds` without a key the
// screen goes to `dim` % (0: off), `brightness` % while on. Without a content: scrOff / scrBright
void screenSleep(uint32_t seconds, uint8_t brightness, uint8_t dim);

// a key: wakes a blanked screen (and does nothing else), takes the QR page away, else goes to the content (the lines:
// prev / next page)
void screenKey(ScreenKey key);

// the Matter pairing window through the matter module and its QR code at once (the tile "matter-pair")
bool screenMatterPair();

// the button action "display-on" (button.cpp), the head forwards it (modules_api.cpp): wakes the screen up
void displayOn();

// The tile pages (screen_pages.cpp, /nspanel.json): an action of another module on a tile ("action": "<name>", not
// toggle / on / off), called on a tap from the loop of the display driver. 'name' is kept as given (a string literal):
// [a-z0-9-_.], at most 24 characters. Registered again: replaced. The screen's own: "matter-pair" (screenMatterPair())
using PagesAction = void (*)();
void pagesAction(const char* name, PagesAction callback);

void pagesSetup();
void screenSetup();
