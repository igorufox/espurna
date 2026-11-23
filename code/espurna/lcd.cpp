/*

LCD MODULE

A colour TFT on the SPI bus of the spi module (spiSCK / spiMOSI / spiMISO): ILI9341, ILI9342 (the M5Stack's) or ST7789,
a panel of the screen module (screen.h: what it shows, the lines, the QR page, blanking, the WebUI mirror are all
there; the nspanel module draws the pages of its config on it). Here only the controller and its settings, nothing
fixed per board:

  lcdType     ili9341 | ili9342 | st7789                        (none: no panel)
  lcdSize     the controller's own width x height                (ili9341 / st7789 240x320, ili9342 320x240)
  lcdRot      0..3, quarter turns                                (0)
  lcdBgr      blue and red swapped (the ILI934x panels are BGR)  (ili934x 1, st7789 0)
  lcdInv      the colours inverted (most ST7789, newer M5Stacks) (ili934x 0, st7789 1)
  lcdCS       chip select                                        (none: no panel)
  lcdDC       data / command                                     (none: no panel)
  lcdRst      reset, none if the board has none
  lcdBL       backlight, on when high, none if always on; dimmed by PWM when the light_pwm module is there
  lcdMHz      the SPI clock                                      (40; Beken: its software SPI as fast as it goes)

The M5Stack Basic / Gray: lcdType ili9342, lcdCS 14, lcdDC 27, lcdRst 33, lcdBL 32 (spiSCK 18, spiMOSI 23, spiMISO 19,
the ESP32's defaults), the buttons A / B / C on GPIO 39 / 38 / 37.

No Arduino layer: the chip's SPI master of the spi module (chip_sdk.h spi_*), the pins of pins.h.

*/

#include "espurna.h"

#if LCD_SUPPORT

#include "gpio.h"
#include "lcd.h"
#include "pins.h"
#include "screen.h"
#include "spi.h"

#if WEB_SUPPORT
#include "json.h"
#include "ws.h"
#endif

#include <chip_sdk.h>

#include <algorithm>
#include <cstring>

namespace espurna {
namespace lcd {
namespace {

namespace chip = ::espurna::core::chip;

enum class Type {
    None,
    Ili9341,
    Ili9342,
    St7789,
};

struct Config {
    Type type { Type::None };
    int width { 0 };            // the controller's own
    int height { 0 };
    uint8_t rotation { 0 };
    bool bgr { false };
    bool invert { false };
    unsigned char cs { GPIO_NONE };
    unsigned char dc { GPIO_NONE };
    unsigned char rst { GPIO_NONE };
    unsigned char bl { GPIO_NONE };
    uint32_t frequency { 40000000 };
};

// MADCTL: MY 0x80, MX 0x40, MV 0x20, BGR 0x08. The quarter turns of a panel: none, MV|MX, MY|MX, MV|MY, over the
// controller's own way (the ILI9341 is mirrored: MX)
constexpr uint8_t Turns[4] { 0x00, 0x60, 0xc0, 0xa0 };

// the ILI934x power-up (the values of their datasheet example and of the M5Stack's), then the common part
constexpr uint8_t IliInit[] {
    0xef, 3, 0x03, 0x80, 0x02,
    0xcf, 3, 0x00, 0xc1, 0x30,
    0xed, 4, 0x64, 0x03, 0x12, 0x81,
    0xe8, 3, 0x85, 0x00, 0x78,
    0xcb, 5, 0x39, 0x2c, 0x00, 0x34, 0x02,
    0xf7, 1, 0x20,
    0xea, 2, 0x00, 0x00,
    0xc0, 1, 0x23,              // power control 1
    0xc1, 1, 0x10,              // power control 2
    0xc5, 2, 0x3e, 0x28,        // VCOM 1
    0xc7, 1, 0x86,              // VCOM 2
    0xb1, 2, 0x00, 0x13,        // frame rate
    0xb6, 3, 0x08, 0x82, 0x27,  // display function
    0xf2, 1, 0x00,              // no 3 gamma
    0x26, 1, 0x01,              // gamma curve 1
    0xe0, 15, 0x0f, 0x31, 0x2b, 0x0c, 0x0e, 0x08, 0x4e, 0xf1, 0x37, 0x07, 0x10, 0x03, 0x0e, 0x09, 0x00,
    0xe1, 15, 0x00, 0x0e, 0x14, 0x03, 0x11, 0x07, 0x31, 0xc1, 0x48, 0x08, 0x0f, 0x0c, 0x31, 0x36, 0x0f,
    0x00,
};

class Panel final : public ScreenPanel {
public:
    explicit Panel(const Config& config) :
        _config(config)
    {
        const bool turned = (config.rotation & 1) != 0;
        _width = turned ? config.height : config.width;
        _height = turned ? config.width : config.height;
    }

    bool begin() {
        select(false);
        espurna::pins::write(_config.dc, true);

        if (_config.rst != GPIO_NONE) {
            espurna::pins::write(_config.rst, true);
            espurna::time::blockingDelay(espurna::duration::Milliseconds(5));
            espurna::pins::write(_config.rst, false);
            espurna::time::blockingDelay(espurna::duration::Milliseconds(20));
            espurna::pins::write(_config.rst, true);
            espurna::time::blockingDelay(espurna::duration::Milliseconds(150));
        }

        command(0x01);              // software reset
        espurna::time::blockingDelay(espurna::duration::Milliseconds(150));

        if (_config.type != Type::St7789) {
            for (const uint8_t* p = IliInit; *p;) {
                const uint8_t cmd = *p++;
                const uint8_t count = *p++;
                command(cmd, p, count);
                p += count;
            }
        }

        const uint8_t pixel { 0x55 };   // 16 bits a pixel
        command(0x3a, &pixel, 1);

        uint8_t madctl = Turns[_config.rotation & 3];
        if (_config.type == Type::Ili9341) {
            madctl ^= 0x40;
        }
        if (_config.bgr) {
            madctl |= 0x08;
        }
        command(0x36, &madctl, 1);

        command(_config.invert ? 0x21 : 0x20);
        command(0x11);              // out of sleep
        espurna::time::blockingDelay(espurna::duration::Milliseconds(120));
        command(0x13);              // normal mode
        command(0x29);              // display on

        // read back only when the controller answers its ID (an unwired SDO reads all ones: the M5Stack)
        uint8_t id[4] {};
        query(0x04, id, sizeof(id));
        _readable = !((id[1] == 0xff) && (id[2] == 0xff) && (id[3] == 0xff))
            && !((id[1] == 0x00) && (id[2] == 0x00) && (id[3] == 0x00));

        fill(0, 0, _width, _height, ScreenBlack);
        backlight(100);
        return true;
    }

    const char* name() const override {
        switch (_config.type) {
        case Type::Ili9341:
            return "ili9341";
        case Type::Ili9342:
            return "ili9342";
        case Type::St7789:
            return "st7789";
        case Type::None:
            break;
        }

        return "lcd";
    }

    int width() const override {
        return _width;
    }

    int height() const override {
        return _height;
    }

    bool colour() const override {
        return true;
    }

    void fill(int x, int y, int w, int h, uint16_t colour) override {
        if ((w <= 0) || (h <= 0)) {
            return;
        }

        uint8_t buffer[Chunk];
        const uint8_t high = colour >> 8;
        const uint8_t low = colour & 0xff;
        for (size_t index = 0; index < sizeof(buffer); index += 2) {
            buffer[index] = high;
            buffer[index + 1] = low;
        }

        window(x, y, w, h);
        size_t left = static_cast<size_t>(w) * static_cast<size_t>(h) * 2;
        while (left) {
            const size_t part = std::min(left, sizeof(buffer));
            chip::spi_transfer(buffer, nullptr, part);
            left -= part;
        }
        select(false);
    }

    void bits(int x, int y, int w, int h, const uint8_t* rows, uint16_t fore, int32_t back) override {
        if ((w <= 0) || (h <= 0)) {
            return;
        }

        // clipped to the panel: the screen clips boxes, not the glyphs and icons
        const int x0 = std::max(x, 0);
        const int y0 = std::max(y, 0);
        const int x1 = std::min(x + w, _width);
        const int y1 = std::min(y + h, _height);
        if ((x0 >= x1) || (y0 >= y1)) {
            return;
        }

        const int stride = (w + 7) / 8;
        const auto bit = [&](int row, int column) {
            return ((rows[row * stride + column / 8] >> (7 - (column % 8))) & 1) != 0;
        };

        // with a background: the whole box, one window, its rows one after the other
        if (back >= 0) {
            uint8_t line[Chunk];
            const int width = x1 - x0;
            const int per = static_cast<int>(sizeof(line) / 2);
            window(x0, y0, width, y1 - y0);
            for (int row = y0 - y; row < y1 - y; ++row) {
                for (int from = 0; from < width; from += per) {
                    const int count = std::min(per, width - from);
                    for (int index = 0; index < count; ++index) {
                        const uint16_t value = bit(row, x0 - x + from + index) ? fore : static_cast<uint16_t>(back);
                        line[index * 2] = value >> 8;
                        line[index * 2 + 1] = value & 0xff;
                    }
                    chip::spi_transfer(line, nullptr, static_cast<size_t>(count) * 2);
                }
            }
            select(false);
            return;
        }

        // without: the runs of set bits, a window each
        for (int row = y0 - y; row < y1 - y; ++row) {
            int column = x0 - x;
            while (column < x1 - x) {
                if (!bit(row, column)) {
                    ++column;
                    continue;
                }

                const int start = column;
                while ((column < x1 - x) && bit(row, column)) {
                    ++column;
                }
                fill(x + start, y + row, column - start, 1, fore);
            }
        }
    }

    // RAMRD: 18 bits a pixel (R, G, B in the high 6 bits of a byte each) after a dummy byte, at a read clock the
    // controllers take (ILI934x: 6.6 MHz)
    bool read(int x, int y, int w, int h, uint16_t* out) override {
        if (!_readable || (w <= 0) || (h <= 0) || (x < 0) || (y < 0) || (x + w > _width) || (y + h > _height)) {
            return false;
        }

        const uint16_t x1 = static_cast<uint16_t>(x + w - 1);
        const uint16_t y1 = static_cast<uint16_t>(y + h - 1);
        const uint8_t columns[] {
            static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(x & 0xff),
            static_cast<uint8_t>(x1 >> 8), static_cast<uint8_t>(x1 & 0xff),
        };
        const uint8_t rows[] {
            static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y & 0xff),
            static_cast<uint8_t>(y1 >> 8), static_cast<uint8_t>(y1 & 0xff),
        };
        command(0x2a, columns, sizeof(columns));
        command(0x2b, rows, sizeof(rows));

        const uint8_t cmd { 0x2e };
        select(true);
        espurna::pins::write(_config.dc, false);
        chip::spi_transfer(&cmd, nullptr, 1);
        espurna::pins::write(_config.dc, true);
        chip::spi_config(ReadFrequency, 0, true);

        alignas(4) uint8_t zero[ReadChunk] {};
        alignas(4) uint8_t in[ReadChunk];
        chip::spi_transfer(zero, in, 1);   // the dummy byte

        size_t left = static_cast<size_t>(w) * static_cast<size_t>(h);
        while (left) {
            const size_t count = std::min(left, sizeof(in) / 3);
            chip::spi_transfer(zero, in, count * 3);
            for (size_t index = 0; index < count; ++index) {
                const uint8_t r = in[index * 3];
                const uint8_t g = in[index * 3 + 1];
                const uint8_t b = in[index * 3 + 2];
                *out++ = static_cast<uint16_t>(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
            }
            left -= count;
        }

        select(false);
        chip::spi_config(_config.frequency, 0, true);
        return true;
    }

    // a read command's answer (RDDID 0x04, RDDST 0x09 ...): `size` bytes after the command, at the read clock
    void query(uint8_t cmd, uint8_t* out, size_t size) {
        chip::spi_config(_config.frequency, 0, true);
        select(true);
        espurna::pins::write(_config.dc, false);
        chip::spi_transfer(&cmd, nullptr, 1);
        espurna::pins::write(_config.dc, true);
        chip::spi_config(ReadFrequency, 0, true);
        alignas(4) uint8_t zero[16] {};
        alignas(4) uint8_t in[16] {};
        size = std::min(size, sizeof(in));
        chip::spi_transfer(zero, in, size);
        std::memcpy(out, in, size);
        select(false);
        chip::spi_config(_config.frequency, 0, true);
    }

    void power(bool on) override {
        command(on ? 0x29 : 0x28);
        backlight(on ? _brightness : 0);
    }

    void brightness(uint8_t percent) override {
        _brightness = percent;
        backlight(percent);
    }

private:
    static constexpr size_t Chunk { 512 };
    static constexpr size_t ReadChunk { 192 };
    static constexpr uint32_t ReadFrequency { 6000000 };

    void select(bool value) {
        espurna::pins::write(_config.cs, !value);
    }

    void command(uint8_t cmd, const uint8_t* data = nullptr, size_t size = 0) {
        chip::spi_config(_config.frequency, 0, true);
        select(true);
        espurna::pins::write(_config.dc, false);
        chip::spi_transfer(&cmd, nullptr, 1);
        espurna::pins::write(_config.dc, true);
        if (size) {
            chip::spi_transfer(data, nullptr, size);
        }
        select(false);
    }

    // the window of the next pixels, the chip stays selected for them (select(false) after)
    void window(int x, int y, int w, int h) {
        const uint16_t x1 = static_cast<uint16_t>(x + w - 1);
        const uint16_t y1 = static_cast<uint16_t>(y + h - 1);
        const uint8_t columns[] {
            static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(x & 0xff),
            static_cast<uint8_t>(x1 >> 8), static_cast<uint8_t>(x1 & 0xff),
        };
        const uint8_t rows[] {
            static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y & 0xff),
            static_cast<uint8_t>(y1 >> 8), static_cast<uint8_t>(y1 & 0xff),
        };

        command(0x2a, columns, sizeof(columns));
        command(0x2b, rows, sizeof(rows));

        const uint8_t write { 0x2c };
        select(true);
        espurna::pins::write(_config.dc, false);
        chip::spi_transfer(&write, nullptr, 1);
        espurna::pins::write(_config.dc, true);
    }

    void backlight(uint8_t percent) {
        if (_config.bl == GPIO_NONE) {
            return;
        }

        if (chip::pwm_bus && (percent > 0) && (percent < 100)) {
            espurna::pins::pwm(_config.bl, percent, 100, 5000);
            _pwm = true;
            return;
        }

        if (_pwm) {
            espurna::pins::pwm(_config.bl, 0, 100, 5000);
            espurna::pins::mode(_config.bl, espurna::pins::Output);
            _pwm = false;
        }
        espurna::pins::write(_config.bl, percent > 0);
    }

    Config _config;
    int _width { 0 };
    int _height { 0 };
    uint8_t _brightness { 100 };
    bool _pwm { false };
    bool _readable { false };
};

Panel* panel { nullptr };

Type parse_type(const espurna::string& value) {
    if (value.equalsIgnoreCase(STRING_VIEW("ili9341"))) {
        return Type::Ili9341;
    }
    if (value.equalsIgnoreCase(STRING_VIEW("ili9342"))) {
        return Type::Ili9342;
    }
    if (value.equalsIgnoreCase(STRING_VIEW("st7789"))) {
        return Type::St7789;
    }
    return Type::None;
}

Config read_config() {
    Config out;
    out.type = parse_type(getSetting("lcdType"));

    const bool ili = (out.type == Type::Ili9341) || (out.type == Type::Ili9342);
    out.width = (out.type == Type::Ili9342) ? 320 : 240;
    out.height = (out.type == Type::Ili9342) ? 240 : 320;

    const auto size = getSetting("lcdSize");
    const auto x = size.indexOf('x');
    if (x > 0) {
        const int width = size.substring(0, x).toInt();
        const int height = size.substring(x + 1).toInt();
        if ((width > 0) && (width <= 480) && (height > 0) && (height <= 480)) {
            out.width = width;
            out.height = height;
        }
    }

    out.rotation = static_cast<uint8_t>(getSetting("lcdRot", 0) & 3);
    out.bgr = getSetting("lcdBgr", ili);
    out.invert = getSetting("lcdInv", !ili);
    out.cs = getSetting("lcdCS", static_cast<unsigned char>(GPIO_NONE));
    out.dc = getSetting("lcdDC", static_cast<unsigned char>(GPIO_NONE));
    out.rst = getSetting("lcdRst", static_cast<unsigned char>(GPIO_NONE));
    out.bl = getSetting("lcdBL", static_cast<unsigned char>(GPIO_NONE));
    out.frequency = std::max(1ul, std::min(80ul, getSetting("lcdMHz", 40ul))) * 1000000ul;

    return out;
}

bool output(unsigned char pin, bool level) {
    if (pin == GPIO_NONE) {
        return true;
    }

    if (!gpioValidForOutput(pin) || !gpioLock(pin)) {
        DEBUG_MSG_P(PSTR("[LCD] GPIO%hhu is not free for an output\n"), pin);
        return false;
    }

    espurna::pins::mode(pin, espurna::pins::Output);
    espurna::pins::write(pin, level);
    return true;
}

void start() {
    const auto config = read_config();
    if (config.type == Type::None) {
        return;
    }

    if ((config.cs == GPIO_NONE) || (config.dc == GPIO_NONE)) {
        DEBUG_MSG_P(PSTR("[LCD] No lcdCS / lcdDC, no panel\n"));
        return;
    }

    if (!spiReady()) {
        DEBUG_MSG_P(PSTR("[LCD] The SPI bus is not started (spiSCK / spiMOSI / spiMISO)\n"));
        return;
    }

    if (!output(config.cs, true) || !output(config.dc, true) || !output(config.rst, true)
        || !output(config.bl, false))
    {
        return;
    }

    auto* created = new (std::nothrow) Panel(config);
    if (!created) {
        DEBUG_MSG_P(PSTR("[LCD] No memory for the panel\n"));
        return;
    }

    created->begin();
    panel = created;
    DEBUG_MSG_P(PSTR("[LCD] %s %dx%d, CS GPIO%hhu DC GPIO%hhu, %u MHz\n"), panel->name(), panel->width(),
        panel->height(), config.cs, config.dc, static_cast<unsigned>(config.frequency / 1000000ul));
    screenPanel(panel);
}

#if WEB_SUPPORT
void on_visible(espurna::json::Writer& root) {
    wsPayloadModule(root, STRING_VIEW("lcd"));
}

void on_connected(espurna::json::Writer& root) {
    root.field(STRING_VIEW("lcdType"), getSetting("lcdType"));
    root.field(STRING_VIEW("lcdSize"), getSetting("lcdSize"));
    root.field(STRING_VIEW("lcdRot"), getSetting("lcdRot", 0));
    root.field(STRING_VIEW("lcdBgr"), getSetting("lcdBgr", STRING_VIEW("").toString()));
    root.field(STRING_VIEW("lcdInv"), getSetting("lcdInv", STRING_VIEW("").toString()));
    root.field(STRING_VIEW("lcdCS"), getSetting("lcdCS", static_cast<unsigned char>(GPIO_NONE)));
    root.field(STRING_VIEW("lcdDC"), getSetting("lcdDC", static_cast<unsigned char>(GPIO_NONE)));
    root.field(STRING_VIEW("lcdRst"), getSetting("lcdRst", static_cast<unsigned char>(GPIO_NONE)));
    root.field(STRING_VIEW("lcdBL"), getSetting("lcdBL", static_cast<unsigned char>(GPIO_NONE)));
    root.field(STRING_VIEW("lcdMHz"), getSetting("lcdMHz", 40ul));
    root.field(STRING_VIEW("spiSCK"), getSetting("spiSCK"));
    root.field(STRING_VIEW("spiMOSI"), getSetting("spiMOSI"));
    root.field(STRING_VIEW("spiMISO"), getSetting("spiMISO"));
}

bool on_key_check(espurna::StringView key, espurna::json::Value) {
    return key.startsWith(STRING_VIEW("lcd"));
}
#endif

} // namespace
} // namespace lcd
} // namespace espurna

#if TERMINAL_SUPPORT
namespace espurna {
namespace lcd {
namespace {

PROGMEM_STRING(LcdId, "LCD.ID");

// LCD.ID: what the controller answers to its read commands (whether it is read at all: the MISO line), and the first
// pixels of its video memory
void terminal_id(::terminal::CommandContext&& ctx) {
    if (!panel) {
        terminalError(ctx, STRING_VIEW("no panel"));
        return;
    }

    static constexpr uint8_t Commands[] { 0x04, 0x09, 0x0a, 0x0c, 0xd3, 0xda, 0xdb, 0xdc };
    for (const auto cmd : Commands) {
        uint8_t out[5] {};
        panel->query(cmd, out, sizeof(out));
        ctx.output.printf_P(PSTR("0x%02X: %02X %02X %02X %02X %02X\n"), cmd, out[0], out[1], out[2], out[3], out[4]);
    }

    uint16_t pixels[4] {};
    const bool read = panel->read(0, 0, 4, 1, pixels);
    ctx.output.printf_P(PSTR("pixels (0,0): %s %04X %04X %04X %04X\n"), read ? "read" : "no read", pixels[0], pixels[1],
        pixels[2], pixels[3]);
    terminalOK(ctx);
}

static constexpr ::terminal::Command Commands[] PROGMEM {
    {LcdId, terminal_id},
};

} // namespace
} // namespace lcd
} // namespace espurna
#endif

void lcdSetup() {
    using namespace espurna::lcd;

    // the pins and the controller at boot only: a change applies after a reboot
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
}

#endif // LCD_SUPPORT
