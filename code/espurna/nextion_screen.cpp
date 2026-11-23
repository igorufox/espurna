/*

NEXTION MODULE - the Nextion as a panel of the screen module

The display has pages of its own (the OHpanel TFT): the screen module takes it only for its QR page (the Matter
pairing code, SCREEN.QR, the tile "matter-pair"), on the blank page of the OHpanel driver (nextionScreenShow(),
nextion.h): the boxes as Nextion `fill`, the text lines in the TFT's own fonts (`xstr`), drawn again whenever the driver
redraws (the screen draws from screenPanelDraw()). A touch takes the page away (screenPanelClosed()). Lovelace: no blank
page, the screen says so once in the log.

The QR page (one implementation for every panel) and its rules are in screen.cpp.

*/

#include "espurna.h"

#if NEXTION_SUPPORT

#include "nextion.h"
#include "screen.h"

namespace espurna {
namespace nextion {
namespace {

// the EU design, landscape: the visible part of the 480 x 320 (ref. nextion_ohpanel.cpp, other::Width)
constexpr int Width { 450 };
constexpr int Height { 320 };

void draw(int, int) {
    screenPanelDraw();
}

void closed() {
    screenPanelClosed();
}

int font_of(ScreenFont font) {
    switch (font) {
    case ScreenFont::Small:
        return 0;
    case ScreenFont::Normal:
        return 1;
    case ScreenFont::Large:
        return 2;
    case ScreenFont::Big:
    case ScreenFont::Huge:
        break;
    }

    return 3;
}

class Panel final : public ScreenPanel {
public:
    const char* name() const override {
        return "nextion";
    }

    int width() const override {
        return Width;
    }

    int height() const override {
        return Height;
    }

    bool colour() const override {
        return true;
    }

    bool ownPages() const override {
        return true;
    }

    bool show() override {
        return nextionScreenShow(draw, closed) == 0;
    }

    void hide() override {
        nextionScreenHide();
    }

    bool shown() const override {
        return nextionScreenShown();
    }

    void fill(int x, int y, int w, int h, uint16_t colour) override {
        nextionScreenFill(x, y, w, h, colour);
    }

    // the screen's page is drawn on white (the QR page): a text without a background gets white
    bool text(int x, int y, int w, int h, ScreenFont font, uint16_t fore, int32_t back, ScreenAlign align,
        const char* value) override
    {
        const int centre = (align == ScreenAlign::Left) ? 0 : (align == ScreenAlign::Center) ? 1 : 2;
        nextionScreenText(x, y, w, h, font_of(font), fore, (back >= 0) ? static_cast<uint16_t>(back) : ScreenWhite,
            centre, value);
        return true;
    }
};

Panel panel;

} // namespace
} // namespace nextion
} // namespace espurna

void nextionScreenSetup() {
    screenPanel(&espurna::nextion::panel);
}

#endif // NEXTION_SUPPORT
