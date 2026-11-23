/*

OLED MODULE

The small I2C screen of a board: SSD1306 or SH1106 on the bus of the i2c module (i2cSDA / i2cSCL), a panel of the
screen module (screen.h: what it shows, the lines, the QR page, blanking, the WebUI mirror are all there). Here only the
controller and its settings, nothing fixed per board:

  oledType    ssd1306 | sh1106                       (ssd1306)
  oledAddr    the address on the bus, 60 or 0x3c       (0x3c)
  oledRst     the reset pin of the panel, held high    (none; Heltec WiFi Kit 32: 16)
  oledSize    128x64 | 128x32 | 64x48 | 64x32          (128x64)
  oledFlip    turned by 180 degrees                    (0)

The terminal `OLED` prints what the screen shows (its pixels; not the QR page).

No Arduino layer: the controller commands go out with i2c_write() of the chip (arch/rtos/chip_sdk.h) on the bus the
i2c module started; the screen module draws into the pixel buffer here, only the changed pages of 8 rows are sent.

*/

#include "espurna.h"

#if OLED_SUPPORT

#include "gpio.h"
#include "oled.h"
#include "pins.h"
#include "screen.h"

#if WEB_SUPPORT
#include "json.h"
#include "ws.h"
#endif

#include <chip_sdk.h>

#include <cstring>
#include <memory>
#include <new>

namespace espurna {
namespace oled {
namespace {

enum class Geometry {
    W128H64,
    W128H32,
    W64H48,
    W64H32,
};

namespace chip = ::espurna::core::chip;

// a colour of the screen lights a pixel when it is brighter than half (the luma of its 8-bit channels)
bool lit(uint16_t colour) {
    const int r = ((colour >> 11) & 0x1f) << 3;
    const int g = ((colour >> 5) & 0x3f) << 2;
    const int b = (colour & 0x1f) << 3;
    return (r * 30 + g * 59 + b * 11) >= (128 * 100);
}

// SSD1306 / SH1106 on I2C: the commands after a 0x00 control byte, the pixels after 0x40. The pixel buffer as the
// controllers keep it: a byte is a column of 8 rows (the lowest bit on top), the pages of 8 rows one after the other
class Panel final : public ScreenPanel {
public:
    static constexpr size_t Max { 128 * 64 / 8 };

    bool begin(bool sh1106, uint8_t address, Geometry geometry, bool flip) {
        _sh1106 = sh1106;
        _address = address;

        switch (geometry) {
        case Geometry::W128H64:
            _width = 128;
            _height = 64;
            break;
        case Geometry::W128H32:
            _width = 128;
            _height = 32;
            break;
        case Geometry::W64H48:
            _width = 64;
            _height = 48;
            break;
        case Geometry::W64H32:
            _width = 64;
            _height = 32;
            break;
        }

        // the RAM of the controllers is 128 (SSD1306) / 132 (SH1106) columns wide, the narrow panels sit in its middle
        _column = static_cast<uint8_t>(((128 - _width) / 2) + (_sh1106 ? 2 : 0));

        const uint8_t compins = ((_width == 128) && (_height == 32)) ? 0x02 : 0x12;
        const uint8_t init[] {
            0xae,                   // off
            0xd5, 0xf0,             // the clock
            0xa8, static_cast<uint8_t>(_height - 1),
            0xd3, 0x00,             // no offset
            0x40,                   // start line 0
            0x8d, 0x14,             // the charge pump (SSD1306; SH1106 ignores it)
            0x20, 0x00,             // horizontal addressing (SSD1306)
            static_cast<uint8_t>(flip ? 0xa0 : 0xa1),
            static_cast<uint8_t>(flip ? 0xc0 : 0xc8),
            0xda, compins,
            0x81, 0xcf,             // contrast
            0xd9, 0xf1,             // pre-charge
            0xdb, 0x40,             // vcomh
            0xa4,                   // the RAM on the screen
            0xa6,                   // not inverted
            0x2e,                   // no scrolling
        };

        if (!commands(init, sizeof(init))) {
            return false;
        }

        std::memset(_pixels, 0, sizeof(_pixels));
        std::memset(_sent, 0xff, sizeof(_sent));
        flush();

        return on(true);
    }

    const char* name() const override {
        return _sh1106 ? "sh1106" : "ssd1306";
    }

    int width() const override {
        return _width;
    }

    int height() const override {
        return _height;
    }

    bool colour() const override {
        return false;
    }

    void fill(int x, int y, int w, int h, uint16_t colour) override {
        const bool value = lit(colour);
        for (int row = y; row < y + h; ++row) {
            for (int column = x; column < x + w; ++column) {
                set(column, row, value);
            }
        }
    }

    void bits(int x, int y, int w, int h, const uint8_t* rows, uint16_t fore, int32_t back) override {
        const bool on = lit(fore);
        const bool off = (back >= 0) && lit(static_cast<uint16_t>(back));
        const int stride = (w + 7) / 8;
        for (int row = 0; row < h; ++row) {
            const uint8_t* line = rows + row * stride;
            for (int column = 0; column < w; ++column) {
                if ((line[column / 8] >> (7 - (column % 8))) & 1) {
                    set(x + column, y + row, on);
                } else if (back >= 0) {
                    set(x + column, y + row, off);
                }
            }
        }
    }

    // the pages of 8 rows that changed since the last time go to the screen
    void flush() override {
        const int pages = (_height + 7) / 8;
        for (int page = 0; page < pages; ++page) {
            const auto* row = &_pixels[page * _width];
            auto* sent = &_sent[page * _width];
            if (std::memcmp(row, sent, static_cast<size_t>(_width)) == 0) {
                continue;
            }

            if (!page_at(page) || !data(row, static_cast<size_t>(_width))) {
                return;
            }

            std::memcpy(sent, row, static_cast<size_t>(_width));
        }
    }

    void power(bool value) override {
        on(value);
    }

    void brightness(uint8_t percent) override {
        const uint8_t command[] { 0x81, static_cast<uint8_t>((static_cast<unsigned>(percent) * 255u) / 100u) };
        commands(command, sizeof(command));
    }

    // from the buffer: what was sent is what it shows
    bool read(int x, int y, int w, int h, uint16_t* out) override {
        if ((x < 0) || (y < 0) || (x + w > _width) || (y + h > _height)) {
            return false;
        }

        for (int row = y; row < y + h; ++row) {
            for (int column = x; column < x + w; ++column) {
                *out++ = get(column, row) ? 0xffff : 0x0000;
            }
        }

        return true;
    }

    bool get(int x, int y) const {
        return (_pixels[x + (y / 8) * _width] >> (y & 7)) & 1;
    }

private:
    bool on(bool value) {
        const uint8_t command { static_cast<uint8_t>(value ? 0xaf : 0xae) };
        return commands(&command, 1);
    }

    void set(int x, int y, bool value) {
        if ((x < 0) || (y < 0) || (x >= _width) || (y >= _height)) {
            return;
        }

        auto& byte = _pixels[x + (y / 8) * _width];
        const auto mask = static_cast<uint8_t>(1 << (y & 7));
        byte = value ? (byte | mask) : (byte & ~mask);
    }

    bool send(uint8_t control, const uint8_t* values, size_t size) {
        uint8_t buffer[1 + 32];
        buffer[0] = control;
        while (size) {
            const size_t part = std::min(size, sizeof(buffer) - 1);
            std::memcpy(&buffer[1], values, part);
            if (chip::i2c_write(_address, buffer, part + 1, true) != 0) {
                return false;
            }

            values += part;
            size -= part;
        }

        return true;
    }

    bool commands(const uint8_t* values, size_t size) {
        return send(0x00, values, size);
    }

    bool data(const uint8_t* values, size_t size) {
        return send(0x40, values, size);
    }

    bool page_at(int page) {
        if (_sh1106) {
            const uint8_t window[] {
                static_cast<uint8_t>(0xb0 | page),
                static_cast<uint8_t>(_column & 0x0f),
                static_cast<uint8_t>(0x10 | (_column >> 4)),
            };
            return commands(window, sizeof(window));
        }

        const uint8_t window[] {
            0x21, _column, static_cast<uint8_t>(_column + _width - 1),
            0x22, static_cast<uint8_t>(page), static_cast<uint8_t>(page),
        };
        return commands(window, sizeof(window));
    }

    bool _sh1106 { false };
    uint8_t _address { 0x3c };
    uint8_t _column { 0 };
    int _width { 128 };
    int _height { 64 };
    uint8_t _pixels[Max] {};
    uint8_t _sent[Max] {};
};

struct Config {
    bool sh1106 { false };
    uint8_t address { 0x3c };
    unsigned char reset { GPIO_NONE };
    Geometry geometry { Geometry::W128H64 };
    bool flip { false };
};

struct State {
    Config config;
    std::unique_ptr<Panel> panel;
    bool registered { false };
};

State state;

Geometry parse_geometry(const espurna::string& value) {
    if (value.equals(STRING_VIEW("128x32"))) {
        return Geometry::W128H32;
    }
    if (value.equals(STRING_VIEW("64x48"))) {
        return Geometry::W64H48;
    }
    if (value.equals(STRING_VIEW("64x32"))) {
        return Geometry::W64H32;
    }
    return Geometry::W128H64;
}

Config read_config() {
    Config out;
    out.sh1106 = getSetting("oledType").equalsIgnoreCase(STRING_VIEW("sh1106"));

    const auto address = getSetting("oledAddr");
    if (address.length()) {
        out.address = static_cast<uint8_t>(strtoul(address.c_str(), nullptr, 0));
    }

    out.reset = getSetting("oledRst", static_cast<unsigned char>(GPIO_NONE));
    out.geometry = parse_geometry(getSetting("oledSize"));
    out.flip = getSetting("oledFlip", false);

    return out;
}

void reset_pin(unsigned char pin) {
    if ((pin == GPIO_NONE) || !gpioValidForOutput(pin) || !gpioLock(pin)) {
        return;
    }

    espurna::pins::mode(pin, espurna::pins::Output);
    espurna::pins::write(pin, false);
    espurna::time::blockingDelay(espurna::duration::Milliseconds(10));
    espurna::pins::write(pin, true);
    espurna::time::blockingDelay(espurna::duration::Milliseconds(10));
}

void start() {
    const auto previous = state.config;
    state.config = read_config();

    const bool same_panel = state.panel
        && (previous.sh1106 == state.config.sh1106)
        && (previous.address == state.config.address)
        && (previous.reset == state.config.reset)
        && (previous.geometry == state.config.geometry)
        && (previous.flip == state.config.flip);
    if (same_panel) {
        return;
    }

    if (state.registered) {
        screenPanel(nullptr);
        state.registered = false;
    }

    if (previous.reset != GPIO_NONE) {
        gpioUnlock(previous.reset);
    }

    state.panel.reset();
    reset_pin(state.config.reset);

    std::unique_ptr<Panel> panel(new (std::nothrow) Panel());
    if (!panel) {
        DEBUG_MSG_P(PSTR("[OLED] No memory for the panel\n"));
        return;
    }

    if (!panel->begin(state.config.sh1106, state.config.address, state.config.geometry, state.config.flip)) {
        DEBUG_MSG_P(PSTR("[OLED] Nothing answers at 0x%02X (oledAddr, i2cSDA / i2cSCL, oledRst)\n"),
            state.config.address);
        return;
    }

    state.panel = std::move(panel);
    DEBUG_MSG_P(PSTR("[OLED] %s at 0x%02X, %dx%d\n"),
        state.config.sh1106 ? "SH1106" : "SSD1306", state.config.address,
        state.panel->width(), state.panel->height());
    state.registered = screenPanel(state.panel.get());
}

// --- the pixels: the terminal

#if TERMINAL_SUPPORT
PROGMEM_STRING(Oled, "OLED");

void terminal_oled(::terminal::CommandContext&& ctx) {
    if (!state.panel) {
        terminalError(ctx, STRING_VIEW("no screen (oledAddr, i2cSDA / i2cSCL, oledRst)"));
        return;
    }

    const auto& panel = *state.panel;
    ctx.output.printf_P(PSTR("%s 0x%02X %dx%d, %s\n"),
        state.config.sh1106 ? "SH1106" : "SSD1306", state.config.address,
        panel.width(), panel.height(), state.registered ? "the panel of the screen" : "not the screen's panel");

    // the QR page carries the setup passcode of Matter: on the screen only, never in a terminal or a log
    if (state.registered && screenQrShown()) {
        ctx.output.printf_P(PSTR("(the QR page, not printed)\n"));
        terminalOK(ctx);
        return;
    }

    if (!screenOn()) {
        ctx.output.printf_P(PSTR("(blanked)\n"));
    }

    // two rows of pixels per text line: ' ' none, '\'' the upper one, '.' the lower one, ':' both
    espurna::string row;
    for (int y = 0; y < panel.height(); y += 2) {
        row.clear();
        for (int x = 0; x < panel.width(); ++x) {
            const bool upper = panel.get(x, y);
            const bool lower = (y + 1 < panel.height()) && panel.get(x, y + 1);
            row += upper ? (lower ? ':' : '\'') : (lower ? '.' : ' ');
        }
        ctx.output.printf_P(PSTR("|%s|\n"), row.c_str());
    }

    terminalOK(ctx);
}

static constexpr ::terminal::Command Commands[] PROGMEM {
    {Oled, terminal_oled},
};
#endif

#if WEB_SUPPORT
void on_visible(espurna::json::Writer& root) {
    wsPayloadModule(root, STRING_VIEW("oled"));
}

void on_connected(espurna::json::Writer& root) {
    root.field(STRING_VIEW("oledType"), getSetting("oledType", STRING_VIEW("ssd1306").toString()));
    root.field(STRING_VIEW("oledAddr"), getSetting("oledAddr", STRING_VIEW("0x3c").toString()));
    root.field(STRING_VIEW("oledRst"), getSetting("oledRst", static_cast<unsigned char>(GPIO_NONE)));
    root.field(STRING_VIEW("oledSize"), getSetting("oledSize", STRING_VIEW("128x64").toString()));
    root.field(STRING_VIEW("oledFlip"), getSetting("oledFlip", false));
    root.field(STRING_VIEW("i2cSDA"), getSetting("i2cSDA"));
    root.field(STRING_VIEW("i2cSCL"), getSetting("i2cSCL"));
}

bool on_key_check(espurna::StringView key, espurna::json::Value) {
    return key.startsWith(STRING_VIEW("oled"));
}
#endif

} // namespace
} // namespace oled
} // namespace espurna

void oledSetup() {
    using namespace espurna::oled;

    start();

#if TERMINAL_SUPPORT
    espurna::terminal::add(Commands);
#endif
#if WEB_SUPPORT
    wsRegister()
        .onVisible(on_visible)
        .onConnected(on_connected)
        .onKeyCheck(on_key_check);
#endif

    espurnaRegisterReload(start);
}

#endif // OLED_SUPPORT
