/*

OLED MODULE

The small I2C screen of a board: SSD1306 or SH1106 on the bus of the i2c module (i2cSDA / i2cSCL), everything else in
the settings, nothing fixed per board:

  oledType    ssd1306 | sh1106                       (ssd1306)
  oledAddr    the address on the bus, 60 or 0x3c       (0x3c)
  oledRst     the reset pin of the panel, held high    (none; Heltec WiFi Kit 32: 16)
  oledSize    128x64 | 128x32 | 64x48 | 64x32          (128x64)
  oledFlip    turned by 180 degrees                    (0)
  oledFont    10 | 16 | 24, the height of a line       (16)
  oledBright  0..255                                   (as the panel starts)
  oledOff     seconds to blank the screen, 0 - never   (0; the button action "display-on" wakes it)
  oledPage    seconds a page stays                     (5)
  oledLbl0..7 the label of a line, on the left
  oledSrc0..7 what the line shows, on the right:
                time | date | datetime | ip | ssid | rssi | host | uptime | heap | wifi | mqtt
                relay:<id>             ON / OFF of the relay
                sensor:<index>         a magnitude with its units, sensor:<type> - the first of the type (temperature)
                mqtt:<topic>[#<key>]   the last payload of the topic (the full topic, + and # work), a key of its JSON
              a line with a label and no source is the label alone; the lines that do not fit are the next pages

  oledMatter  the Matter pairing QR code while its window is open   (1)

The QR page: while the pairing window of the matter module is open (MATTER.WINDOW, the MATTER panel, a controller
asking) the screen shows the QR code of the pairing payload on the left and the manual code on the right instead of
the lines, a blanked screen wakes up for it; the matter module is reached through the head (matterWindowOpen(),
matterPairingCodes()), without it there is no window and no page. `OLED.QR` shows the Matter code for a minute whatever
the window, `OLED.QR <text>` a QR code of the text, `OLED.QR off` the lines again. The codes carry the setup passcode:
drawn on the screen and the WebUI mirror (the owner's screens, as the MATTER panel), never kept, printed or logged.

The terminal `OLED` prints what the screen shows (its pixels), the OLED panel of the WebUI draws it (oledMirror).

No Arduino layer: the controller commands go out with i2c_write() of the chip (arch/rtos/chip_sdk.h) on the bus the
i2c module started, the pixels are drawn here (the fonts: oled_fonts.h), only the changed pages of 8 rows are sent.

*/

#include "espurna.h"

#if OLED_SUPPORT

#include "espurna_matter.h"
#include "espurna_wifi.h"
#include "json.h"
#include "mqtt.h"
#include "ntp.h"
#include "oled.h"
#include "oled_fonts.h"
#include "pins.h"
#include "relay.h"
#include "sensor.h"
#include "ws.h"

#include <chip_sdk.h>
#include <qrcode.h>

#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <iterator>
#include <memory>
#include <new>

namespace espurna {
namespace oled {
namespace {

static constexpr size_t Lines { 8 };

PROGMEM_STRING(LabelKey, "oledLbl");
PROGMEM_STRING(SourceKey, "oledSrc");

enum class Geometry {
    W128H64,
    W128H32,
    W64H48,
    W64H32,
};

enum class Align {
    Left,
    Center,
    Right,
};

namespace chip = ::espurna::core::chip;

// SSD1306 / SH1106 on I2C: the commands after a 0x00 control byte, the pixels after 0x40. The pixel buffer as the
// controllers keep it: a byte is a column of 8 rows (the lowest bit on top), the pages of 8 rows one after the other
class Screen {
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

        clear();
        resend();
        flush();

        return on(true);
    }

    bool on(bool value) {
        const uint8_t command { static_cast<uint8_t>(value ? 0xaf : 0xae) };
        return commands(&command, 1);
    }

    void brightness(uint8_t value) {
        const uint8_t command[] { 0x81, value };
        commands(command, sizeof(command));
    }

    int width() const {
        return _width;
    }

    int height() const {
        return _height;
    }

    const uint8_t* pixels() const {
        return _pixels;
    }

    void clear() {
        std::memset(_pixels, 0, sizeof(_pixels));
    }

    void set(int x, int y) {
        if ((x < 0) || (y < 0) || (x >= _width) || (y >= _height)) {
            return;
        }

        _pixels[x + (y / 8) * _width] |= static_cast<uint8_t>(1 << (y & 7));
    }

    bool get(int x, int y) const {
        return (_pixels[x + (y / 8) * _width] >> (y & 7)) & 1;
    }

    // the width of the text in pixels with that font
    static int text_width(const uint8_t* font, espurna::StringView text) {
        int out = 0;
        each_char(text, [&](uint8_t code) {
            out += glyph_width(font, code);
        });

        return out;
    }

    void text(int x, int y, const uint8_t* font, espurna::StringView value, Align align) {
        switch (align) {
        case Align::Left:
            break;
        case Align::Center:
            x -= text_width(font, value) / 2;
            break;
        case Align::Right:
            x -= text_width(font, value);
            break;
        }

        const uint8_t height = pgm_read_byte(&font[1]);
        const uint8_t first = pgm_read_byte(&font[2]);
        const uint8_t count = pgm_read_byte(&font[3]);
        const int raster = (height + 7) / 8;
        const uint8_t* table = &font[4];
        const uint8_t* data = &font[4 + count * 4];

        each_char(value, [&](uint8_t code) {
            if ((code < first) || (code >= first + count)) {
                return;
            }

            const uint8_t* entry = &table[(code - first) * 4];
            const uint16_t offset = static_cast<uint16_t>(
                (pgm_read_byte(&entry[0]) << 8) | pgm_read_byte(&entry[1]));
            const uint8_t size = pgm_read_byte(&entry[2]);
            const uint8_t advance = pgm_read_byte(&entry[3]);

            if (offset != 0xffff) {
                for (int index = 0; index < size; ++index) {
                    const uint8_t bits = pgm_read_byte(&data[offset + index]);
                    const int column = x + index / raster;
                    const int row = y + (index % raster) * 8;
                    for (int bit = 0; bit < 8; ++bit) {
                        if (bits & (1 << bit)) {
                            set(column, row + bit);
                        }
                    }
                }
            }

            x += advance;
        });
    }

    // the pages of 8 rows that changed since the last time go to the screen
    void flush() {
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

    // the panel is sent all of its pixels again at the next flush()
    void resend() {
        std::memset(_sent, 0xff, sizeof(_sent));
    }

private:
    template <typename T>
    static void each_char(espurna::StringView text, T&& callback) {
        // UTF-8 of U+00A0..U+00FF to the Latin-1 codes of the fonts, the other multi-byte chars left out
        for (size_t index = 0; index < text.length(); ++index) {
            const auto code = static_cast<uint8_t>(text[index]);
            if (code < 0x80) {
                callback(code);
            } else if (((code == 0xc2) || (code == 0xc3)) && (index + 1 < text.length())) {
                const auto next = static_cast<uint8_t>(text[++index]);
                callback(static_cast<uint8_t>((code == 0xc3) ? (next + 0x40) : next));
            }
        }
    }

    static int glyph_width(const uint8_t* font, uint8_t code) {
        const uint8_t first = pgm_read_byte(&font[2]);
        const uint8_t count = pgm_read_byte(&font[3]);
        if ((code < first) || (code >= first + count)) {
            return 0;
        }

        return pgm_read_byte(&font[4 + (code - first) * 4 + 3]);
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

struct Line {
    espurna::string label;
    espurna::string source;
    espurna::string mqtt_topic;
    espurna::string mqtt_key;
    espurna::string mqtt_value;
};

struct Config {
    bool sh1106 { false };
    uint8_t address { 0x3c };
    unsigned char reset { GPIO_NONE };
    Geometry geometry { Geometry::W128H64 };
    bool flip { false };
    int font { 16 };
    int brightness { -1 };
    uint32_t off_ms { 0 };
    uint32_t page_ms { 5000 };
    bool matter { true };
};

struct State {
    Config config;
    std::unique_ptr<Screen> display;
    Line lines[Lines];
    size_t page { 0 };
    uint32_t page_since { 0 };
    uint32_t on_since { 0 };
    uint32_t last_draw { 0 };
    bool on { true };
    espurna::string drawn;
    bool mirror_dirty { true };
    // OLED.QR: the QR page for a minute, of qr_text or (empty) of the Matter codes
    bool qr_manual { false };
    uint32_t qr_since { 0 };
    espurna::string qr_text;
};

State state;

// --- settings

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

    const auto font = getSetting("oledFont", 16);
    out.font = (font <= 10) ? 10 : (font >= 24) ? 24 : 16;

    out.brightness = getSetting("oledBright", -1);
    out.off_ms = getSetting("oledOff", 0ul) * 1000ul;
    out.page_ms = std::max(1ul, getSetting("oledPage", 5ul)) * 1000ul;
    out.matter = getSetting("oledMatter", true);

    return out;
}

void read_lines() {
    for (size_t index = 0; index < Lines; ++index) {
        auto& line = state.lines[index];
        line.label = getSetting({LabelKey, index});
        line.source = getSetting({SourceKey, index});
        line.source.trim();

        line.mqtt_topic = espurna::string();
        line.mqtt_key = espurna::string();
        line.mqtt_value = espurna::string();

        if (line.source.startsWith(STRING_VIEW("mqtt:"))) {
            auto topic = line.source.substring(5);
            const auto hash = topic.lastIndexOf('#');
            // `#` alone at the end is the wildcard of MQTT, `#key` after a `/`-less tail is the key of the JSON
            if ((hash > 0) && (static_cast<size_t>(hash) + 1 < topic.length()) && (topic[hash - 1] != '/')) {
                line.mqtt_key = topic.substring(hash + 1);
                topic = topic.substring(0, hash);
            }
            line.mqtt_topic = topic;
        }
    }
}

// --- what a line shows

const uint8_t* font_data(int font) {
    switch (font) {
    case 10:
        return fonts::Arial10;
    case 24:
        return fonts::Arial24;
    }

    return fonts::Arial16;
}

int line_pitch(int font) {
    switch (font) {
    case 10:
        return 12;
    case 24:
        return 28;
    }

    return 16;
}

espurna::string two(int value) {
    espurna::string out;
    if (value < 10) {
        out += '0';
    }
    out += espurna::string(value);
    return out;
}

bool local_time(tm& out) {
#if NTP_SUPPORT
    if (!ntpSynced()) {
        return false;
    }
#endif
    const auto now = ::time(nullptr);
    if (now < 1000000000) {
        return false;
    }

    localtime_r(&now, &out);
    return true;
}

espurna::string sensor_value(espurna::StringView spec) {
#if SENSOR_SUPPORT
    const size_t count = magnitudeCount();
    size_t index = count;

    const auto text = spec.toString();
    if (text.length() && isdigit(static_cast<unsigned char>(text[0]))) {
        index = static_cast<size_t>(text.toInt());
    } else {
        for (size_t magnitude = 0; magnitude < count; ++magnitude) {
            if (magnitudeTypeTopic(magnitudeType(magnitude)).equalsIgnoreCase(text)) {
                index = magnitude;
                break;
            }
        }
    }

    if (index >= count) {
        return STRING_VIEW("?").toString();
    }

    const auto value = magnitudeValue(index);
    if (std::isnan(value.value)) {
        return STRING_VIEW("?").toString();
    }

    auto out = value.repr;
    const auto units = magnitudeUnitsName(value.units);
    if (units.length() && !units.equals(STRING_VIEW("none"))) {
        out += ' ';
        out += units;
    }

    return out;
#else
    return STRING_VIEW("?").toString();
#endif
}

espurna::string uptime_text() {
    const auto seconds = systemUptime().count();
    const auto days = seconds / 86400;
    espurna::string out;
    if (days) {
        out += espurna::string(static_cast<unsigned long>(days));
        out += STRING_VIEW("d ");
    }
    out += two(static_cast<int>((seconds / 3600) % 24));
    out += ':';
    out += two(static_cast<int>((seconds / 60) % 60));
    return out;
}

espurna::string value_of(const Line& line) {
    const auto& source = line.source;
    if (!source.length()) {
        return espurna::string();
    }

    tm t{};
    if (source.equals(STRING_VIEW("time"))) {
        return local_time(t)
            ? two(t.tm_hour) + ':' + two(t.tm_min)
            : STRING_VIEW("--:--").toString();
    }

    if (source.equals(STRING_VIEW("date"))) {
        return local_time(t)
            ? two(t.tm_mday) + '.' + two(t.tm_mon + 1) + '.' + espurna::string(t.tm_year + 1900)
            : STRING_VIEW("--.--").toString();
    }

    if (source.equals(STRING_VIEW("datetime"))) {
        return local_time(t)
            ? two(t.tm_mday) + '.' + two(t.tm_mon + 1) + ' ' + two(t.tm_hour) + ':' + two(t.tm_min)
            : STRING_VIEW("--.-- --:--").toString();
    }

    if (source.equals(STRING_VIEW("ip"))) {
        if (!wifiConnected()) {
            return STRING_VIEW("-").toString();
        }
        char ip[16];
        wifiStaIp().toString(ip, sizeof(ip));
        return espurna::string(ip);
    }

    if (source.equals(STRING_VIEW("ssid"))) {
        return wifiStaSsid();
    }

    if (source.equals(STRING_VIEW("rssi"))) {
        return wifiConnected()
            ? espurna::string(static_cast<int>(wifiStaRssi())) + STRING_VIEW(" dBm")
            : STRING_VIEW("-").toString();
    }

    if (source.equals(STRING_VIEW("host"))) {
        return systemHostname();
    }

    if (source.equals(STRING_VIEW("uptime"))) {
        return uptime_text();
    }

    if (source.equals(STRING_VIEW("heap"))) {
        return espurna::string(static_cast<unsigned long>(systemFreeHeap() / 1024)) + STRING_VIEW(" KB");
    }

    if (source.equals(STRING_VIEW("wifi"))) {
        return wifiConnected() ? STRING_VIEW("on").toString() : STRING_VIEW("off").toString();
    }

    if (source.equals(STRING_VIEW("mqtt"))) {
        return mqttConnected() ? STRING_VIEW("on").toString() : STRING_VIEW("off").toString();
    }

    if (source.startsWith(STRING_VIEW("relay:"))) {
#if RELAY_SUPPORT
        const auto id = static_cast<size_t>(source.substring(6).toInt());
        switch (relayStatus(id)) {
        case RelayStatus::On:
            return STRING_VIEW("ON").toString();
        case RelayStatus::Off:
            return STRING_VIEW("OFF").toString();
        case RelayStatus::NotReady:
        case RelayStatus::NotAvailable:
            break;
        }
#endif
        return STRING_VIEW("-").toString();
    }

    if (source.startsWith(STRING_VIEW("sensor:"))) {
        return sensor_value(source.view().slice(7));
    }

    if (line.mqtt_topic.length()) {
        return line.mqtt_value.length() ? line.mqtt_value : STRING_VIEW("?").toString();
    }

    return STRING_VIEW("?").toString();
}

// --- the screen

size_t rows() {
    if (!state.display) {
        return 1;
    }

    return std::max<size_t>(1, static_cast<size_t>(state.display->height() / line_pitch(state.config.font)));
}

size_t used_lines(size_t* out) {
    size_t count = 0;
    for (size_t index = 0; index < Lines; ++index) {
        const auto& line = state.lines[index];
        if (line.label.length() || line.source.length()) {
            out[count++] = index;
        }
    }

    return count;
}

// --- the QR page: the Matter pairing code while its window is open (oledMatter), or OLED.QR

// ECC low, versions 1..5: a 22 character MT: payload is version 1 (21 x 21)
static constexpr uint8_t QrVersionMax { 5 };
static constexpr uint16_t QrAlphanumericLow[QrVersionMax] { 25, 47, 77, 114, 154 };
static constexpr uint16_t QrBytesLow[QrVersionMax] { 17, 32, 53, 78, 106 };
static constexpr int QrSizeMax { 17 + 4 * QrVersionMax };
static constexpr size_t QrBufferSize { ((QrSizeMax * QrSizeMax) + 7) / 8 };
static constexpr uint32_t QrManualMs { 60000 };

uint8_t qr_version(const char* text) {
    bool alphanumeric = true;
    for (const char* p = text; *p; ++p) {
        const char c = *p;
        if (!((c >= '0') && (c <= '9')) && !((c >= 'A') && (c <= 'Z')) && !std::strchr(" $%*+-./:", c)) {
            alphanumeric = false;
            break;
        }
    }

    const auto length = std::strlen(text);
    const auto* capacity = alphanumeric ? QrAlphanumericLow : QrBytesLow;
    for (uint8_t index = 0; index < QrVersionMax; ++index) {
        if (length <= capacity[index]) {
            return index + 1;
        }
    }

    return 0;
}

// the code on the left, as large as the height takes it (white with its quiet zone, the dark modules off), the
// lines of text on the right in the small font. false - the text does not fit a QR code up to version 5
bool draw_qr(Screen& display, const char* payload, const char* const* lines, size_t count) {
    const auto version = qr_version(payload);
    uint8_t buffer[QrBufferSize];
    QRCode code;
    if (!version || (qrcode_initText(&code, buffer, version, ECC_LOW, payload) != 0)) {
        return false;
    }

    // a quiet zone of 4 modules where the screen takes it, else 2
    int quiet = 4;
    int module = display.height() / (code.size + 2 * quiet);
    if (module < 1) {
        quiet = 2;
        module = std::max(1, display.height() / (code.size + 2 * quiet));
    }

    const int side = (code.size + 2 * quiet) * module;
    const int top = (display.height() - side) / 2;

    display.clear();
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const int qx = x / module - quiet;
            const int qy = y / module - quiet;
            const bool dark = (qx >= 0) && (qy >= 0) && (qx < code.size) && (qy < code.size)
                && qrcode_getModule(&code, static_cast<uint8_t>(qx), static_cast<uint8_t>(qy));
            if (!dark) {
                display.set(x, top + y);
            }
        }
    }

    std::memset(buffer, 0, sizeof(buffer));

    const int left = side + 4;
    for (size_t index = 0; index < count; ++index) {
        display.text(left, static_cast<int>(index) * 12, fonts::Arial10, espurna::StringView(lines[index]), Align::Left);
    }

    return true;
}

// true - the QR page is on the screen (the lines wait)
bool draw_qr_page(Screen& display) {
    const uint32_t now = espurna::time::ms();
    const bool manual = state.qr_manual && (now - state.qr_since < QrManualMs);
    if (!manual) {
        state.qr_manual = false;
        state.qr_text = espurna::string();
    }

    const bool window = state.config.matter && matterWindowOpen();
    if (!manual && !window) {
        return false;
    }

    // drawn once: the key has no payload (the codes carry the setup passcode, they are never kept)
    const auto key = state.qr_text.length()
        ? espurna::string("\x01qr:text:") + espurna::string(static_cast<unsigned long>(state.qr_text.length()))
        : espurna::string("\x01qr:matter");
    if (key.equals(state.drawn)) {
        return true;
    }

    bool drawn = false;
    if (state.qr_text.length()) {
        const char* lines[] { "QR" };
        drawn = draw_qr(display, state.qr_text.c_str(), lines, 1);
    } else {
        char payload[32] {};
        char code[24] {};
        if (matterPairingCodes(payload, sizeof(payload), code, sizeof(code))) {
            // "34970112332" -> "3497-011" and "2332" (the width of the right side), as the controller apps print it
            char first[12] {};
            char second[12] {};
            if (std::strlen(code) == 11) {
                snprintf_P(first, sizeof(first), PSTR("%.4s-%.3s"), code, code + 4);
                snprintf_P(second, sizeof(second), PSTR("%.4s"), code + 7);
            } else {
                snprintf_P(first, sizeof(first), PSTR("%.10s"), code);
                snprintf_P(second, sizeof(second), PSTR("%.10s"), (std::strlen(code) > 10) ? code + 10 : "");
            }

            const char* lines[] { "Matter", first, second };
            drawn = draw_qr(display, payload, lines, std::size(lines));
            std::memset(payload, 0, sizeof(payload));
            std::memset(code, 0, sizeof(code));
            std::memset(first, 0, sizeof(first));
            std::memset(second, 0, sizeof(second));
        }
    }

    if (!drawn) {
        display.clear();
        const char* message = state.qr_text.length() ? "QR: too long" : "Matter: no codes";
        display.text(0, 0, fonts::Arial10, espurna::StringView(message), Align::Left);
    }

    state.drawn = key;
    display.flush();
    state.mirror_dirty = true;
    return true;
}

void draw() {
    auto& display = *state.display;

    if (draw_qr_page(display)) {
        return;
    }

    size_t used[Lines];
    const auto count = used_lines(used);
    const auto per_page = rows();
    const auto pages = std::max<size_t>(1, (count + per_page - 1) / per_page);
    if (state.page >= pages) {
        state.page = 0;
    }

    // what the page says: drawn again only when it changed
    espurna::string text;
    espurna::string values[Lines];
    for (size_t row = 0; (row < per_page) && (row < Lines); ++row) {
        const auto position = state.page * per_page + row;
        if (position >= count) {
            break;
        }

        const auto& line = state.lines[used[position]];
        values[row] = value_of(line);
        text += line.label;
        text += '\t';
        text += values[row];
        text += '\n';
    }

    if (!count) {
        text = systemHostname();
    }

    text += espurna::string(static_cast<unsigned long>(state.page));
    if (text.equals(state.drawn)) {
        return;
    }

    state.drawn = text;

    display.clear();
    const auto* font = font_data(state.config.font);
    const int width = display.width();
    const int pitch = line_pitch(state.config.font);

    if (!count) {
        // nothing set yet: the name of the board, so the screen shows it is alive
        const auto host = systemHostname();
        display.text(width / 2, 0, font, host.view(), Align::Center);
    }

    for (size_t row = 0; (row < per_page) && (row < Lines); ++row) {
        const auto position = state.page * per_page + row;
        if (position >= count) {
            break;
        }

        const auto& line = state.lines[used[position]];
        const int y = static_cast<int>(row) * pitch;
        if (line.label.length()) {
            display.text(0, y, font, line.label.view(), Align::Left);
            display.text(width, y, font, values[row].view(), Align::Right);
        } else {
            display.text(0, y, font, values[row].view(), Align::Left);
        }
    }

    if (pages > 1) {
        // the page marks, one dot per page in the right bottom corner
        for (size_t page = 0; page < pages; ++page) {
            const int x = width - 1 - static_cast<int>((pages - 1 - page) * 3);
            const int y = display.height() - 1;
            display.set(x, y);
            if (page == state.page) {
                display.set(x, y - 1);
            }
        }
    }

    display.flush();
    state.mirror_dirty = true;
}

void power(bool on) {
    if (!state.display || (state.on == on)) {
        return;
    }

    state.on = on;
    state.display->on(on);
    if (on) {
        state.on_since = espurna::time::ms();
        state.drawn = espurna::string();
    }

    state.mirror_dirty = true;
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

    const bool same_panel = state.display
        && (previous.sh1106 == state.config.sh1106)
        && (previous.address == state.config.address)
        && (previous.reset == state.config.reset)
        && (previous.geometry == state.config.geometry)
        && (previous.flip == state.config.flip);

    if (!same_panel) {
        if (previous.reset != GPIO_NONE) {
            gpioUnlock(previous.reset);
        }

        state.display.reset();
        reset_pin(state.config.reset);

        std::unique_ptr<Screen> screen(new (std::nothrow) Screen());
        if (!screen) {
            DEBUG_MSG_P(PSTR("[OLED] No memory for the screen\n"));
            return;
        }

        if (!screen->begin(state.config.sh1106, state.config.address, state.config.geometry, state.config.flip)) {
            DEBUG_MSG_P(PSTR("[OLED] Nothing answers at 0x%02X (oledAddr, i2cSDA / i2cSCL, oledRst)\n"),
                state.config.address);
            return;
        }

        state.display = std::move(screen);
        state.on = true;
        DEBUG_MSG_P(PSTR("[OLED] %s at 0x%02X, %dx%d\n"),
            state.config.sh1106 ? "SH1106" : "SSD1306", state.config.address,
            state.display->width(), state.display->height());
    }

    if (state.config.brightness >= 0) {
        state.display->brightness(static_cast<uint8_t>(std::min(state.config.brightness, 255)));
    }

    read_lines();
    state.page = 0;
    state.page_since = espurna::time::ms();
    state.on_since = state.page_since;
    state.drawn = espurna::string();
    power(true);
}

void tick() {
    if (!state.display) {
        return;
    }

    const uint32_t now = espurna::time::ms();

    // the Matter pairing window opened: the QR page on a blanked screen too
    if (!state.on && state.config.matter && matterWindowOpen()) {
        power(true);
    }

    if (state.config.off_ms && state.on && (now - state.on_since > state.config.off_ms)
        && !(state.config.matter && matterWindowOpen())) {
        power(false);
    }

    if (!state.on) {
        return;
    }

    if (now - state.page_since > state.config.page_ms) {
        state.page_since = now;
        ++state.page;
    }

    if (now - state.last_draw < 250) {
        return;
    }

    state.last_draw = now;
    draw();
}

// --- MQTT: the lines of mqtt:<topic>

// the filter of a subscription against a topic: `+` one level, `#` at the end the rest
bool topic_match(espurna::StringView filter, espurna::StringView topic) {
    size_t f = 0;
    size_t t = 0;
    while (f < filter.length()) {
        if (filter[f] == '#') {
            return true;
        }

        if (filter[f] == '+') {
            while ((t < topic.length()) && (topic[t] != '/')) {
                ++t;
            }
            ++f;
            continue;
        }

        if ((t >= topic.length()) || (filter[f] != topic[t])) {
            return false;
        }

        ++f;
        ++t;
    }

    return t == topic.length();
}

void on_mqtt(unsigned int type, espurna::StringView topic, espurna::StringView payload) {
    if (type == MQTT_CONNECT_EVENT) {
        for (const auto& line : state.lines) {
            if (line.mqtt_topic.length()) {
                mqttSubscribeRaw(line.mqtt_topic.c_str());
            }
        }
        return;
    }

    if (type != MQTT_MESSAGE_EVENT) {
        return;
    }

    for (auto& line : state.lines) {
        if (!line.mqtt_topic.length() || !topic_match(line.mqtt_topic.view(), topic)) {
            continue;
        }

        if (!line.mqtt_key.length()) {
            line.mqtt_value = payload.toString();
            continue;
        }

        const auto root = espurna::json::parse(payload);
        if (root.isObject() && root.has(line.mqtt_key.view())) {
            line.mqtt_value = root[line.mqtt_key.view()].asString();
        }
    }
}

// --- the pixels: the terminal, the WebUI

#if TERMINAL_SUPPORT
PROGMEM_STRING(Oled, "OLED");

void terminal_oled(::terminal::CommandContext&& ctx) {
    if (!state.display) {
        terminalError(ctx, STRING_VIEW("no screen (oledAddr, i2cSDA / i2cSCL, oledRst)"));
        return;
    }

    const auto& display = *state.display;
    ctx.output.printf_P(PSTR("%s 0x%02X %dx%d page %zu, %s\n"),
        state.config.sh1106 ? "SH1106" : "SSD1306", state.config.address,
        display.width(), display.height(), state.page,
        state.on ? "on" : "off");

    // the QR page carries the setup passcode of Matter: on the screen only, never in a terminal or a log
    if (state.drawn.startsWith(STRING_VIEW("\x01qr"))) {
        ctx.output.printf_P(PSTR("(the QR page, not printed)\n"));
        terminalOK(ctx);
        return;
    }

    // two rows of pixels per text line: ' ' none, '\'' the upper one, '.' the lower one, ':' both
    espurna::string row;
    for (int y = 0; y < display.height(); y += 2) {
        row.clear();
        for (int x = 0; x < display.width(); ++x) {
            const bool upper = display.get(x, y);
            const bool lower = (y + 1 < display.height()) && display.get(x, y + 1);
            row += upper ? (lower ? ':' : '\'') : (lower ? '.' : ' ');
        }
        ctx.output.printf_P(PSTR("|%s|\n"), row.c_str());
    }

    terminalOK(ctx);
}

PROGMEM_STRING(OledQr, "OLED.QR");

// OLED.QR - the Matter pairing code for a minute (whatever the window), OLED.QR <text> - a QR code of the text,
// OLED.QR off - back to the lines
void terminal_oled_qr(::terminal::CommandContext&& ctx) {
    if (!state.display) {
        terminalError(ctx, STRING_VIEW("no screen"));
        return;
    }

    state.drawn = espurna::string();
    if ((ctx.argv.size() == 2) && ctx.argv[1].equalsIgnoreCase(STRING_VIEW("off"))) {
        state.qr_manual = false;
        state.qr_text = espurna::string();
        terminalOK(ctx);
        return;
    }

    state.qr_manual = true;
    state.qr_since = espurna::time::ms();
    state.qr_text = espurna::string();
    for (size_t index = 1; index < ctx.argv.size(); ++index) {
        if (index > 1) {
            state.qr_text += ' ';
        }
        state.qr_text += ctx.argv[index];
    }

    power(true);
    terminalOK(ctx);
}

static constexpr ::terminal::Command Commands[] PROGMEM {
    {Oled, terminal_oled},
    {OledQr, terminal_oled_qr},
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
    root.field(STRING_VIEW("oledFont"), getSetting("oledFont", 16));
    root.field(STRING_VIEW("oledBright"), getSetting("oledBright", -1));
    root.field(STRING_VIEW("oledOff"), getSetting("oledOff", 0ul));
    root.field(STRING_VIEW("oledPage"), getSetting("oledPage", 5ul));
    root.field(STRING_VIEW("oledMatter"), getSetting("oledMatter", true));
    root.field(STRING_VIEW("i2cSDA"), getSetting("i2cSDA"));
    root.field(STRING_VIEW("i2cSCL"), getSetting("i2cSCL"));

    for (size_t index = 0; index < Lines; ++index) {
        const auto label = espurna::settings::Key(LabelKey, index);
        const auto source = espurna::settings::Key(SourceKey, index);
        root.field(StringView(label.c_str(), label.length()), state.lines[index].label);
        root.field(StringView(source.c_str(), source.length()), state.lines[index].source);
    }

    state.mirror_dirty = true;
}

bool on_key_check(espurna::StringView key, espurna::json::Value) {
    return key.startsWith(STRING_VIEW("oled"));
}

// the pixels for the OLED panel (src/oled.mjs draws them): hex of the buffer, its pages of 8 rows
void mirror() {
    if (!state.mirror_dirty || !state.display || !wsConnected()) {
        return;
    }

    state.mirror_dirty = false;
    const int width = state.display->width();
    const int height = state.display->height();
    const size_t size = static_cast<size_t>(width * ((height + 7) / 8));
    const auto* pixels = state.display->pixels();

    espurna::string hex;
    hex.reserve(size * 2);
    for (size_t index = 0; index < size; ++index) {
        hex += hexEncode(pixels[index]);
    }

    const bool on = state.on;
    wsPost([width, height, on, hex](espurna::json::Writer& root) {
        root.object(STRING_VIEW("oledMirror"));
        root.field(STRING_VIEW("width"), width);
        root.field(STRING_VIEW("height"), height);
        root.field(STRING_VIEW("on"), on);
        root.field(STRING_VIEW("pixels"), hex);
        root.end();
    });
}
#endif

} // namespace
} // namespace oled
} // namespace espurna

void displayOn() {
    using namespace espurna::oled;
    if (!state.display) {
        return;
    }

    state.on_since = espurna::time::ms();
    power(true);
}

void oledSetup() {
    using namespace espurna::oled;

    start();

    mqttRegister(on_mqtt);
#if TERMINAL_SUPPORT
    espurna::terminal::add(Commands);
#endif
#if WEB_SUPPORT
    wsRegister()
        .onVisible(on_visible)
        .onConnected(on_connected)
        .onKeyCheck(on_key_check);
    espurnaRegisterLoop(mirror);
#endif

    espurnaRegisterLoop(tick);
    espurnaRegisterReload(start);
}

#endif // OLED_SUPPORT
