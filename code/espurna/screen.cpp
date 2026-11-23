/*

SCREEN MODULE

What every screen of a board has in common (screen.h): the drawing in the fonts and icons of the module, the pages of
lines, the Matter QR page, blanking, the keys and the WebUI mirror. The panels are the modules that need this one
(oled, lcd, nspanel); one panel at a time.

Settings (one set for every panel):

  scrLbl0..7  the label of a line, on the left
  scrSrc0..7  what the line shows, on the right:
                time | date | datetime | ip | ssid | rssi | host | uptime | heap | wifi | mqtt
                relay:<id>             ON / OFF of the relay
                sensor:<index>         a magnitude with its units, sensor:<type> - the first of the type (temperature)
                mqtt:<topic>[#<key>]   the last payload of the topic (the full topic, + and # work), a key of its JSON
              a line with a label and no source is the label alone; the lines that do not fit are the next pages
  scrFont     10 | 16 | 24, the height class of a line                       (16)
  scrPage     seconds a page of lines stays                                  (5)
  scrOff      seconds to blank the screen, 0 - never                         (0; a key or "display-on" wakes it)
  scrBright   0..100 %                                                       (100)
  scrMatter   the Matter pairing QR code while its window is open            (1)

The oled module had the lines as oledLbl / oledSrc / oledFont / oledPage / oledOff / oledBright / oledMatter: they
become these once (the first boot with the screen module).

The QR page: while the pairing window of the matter module is open (MATTER.WINDOW, the MATTER panel, a controller
asking, the nspanel tile "matter-pair": screenMatterPair()) the screen shows the QR code of the pairing payload and the
manual code over whatever it shows, a blanked screen wakes up for it; a key (or a touch of the NSPanel) takes it away
until the next window. The matter module is reached through the head (matterWindowOpen(), matterPairingCodes()),
without it there is no window and no page. `SCREEN.QR` shows the Matter code for a minute whatever the window,
`SCREEN.QR <text>` a QR code of the text, `SCREEN.QR off` takes it away. The codes carry the setup passcode: drawn on
the screen and the WebUI mirror (the owner's screens, as the MATTER panel), never kept, printed or logged.

The WebUI mirror (the SCREEN panel, src/screen.mjs): what is drawn as a list of drawing commands, the browser draws them
again (its fonts, the MDI font of the icons); an own-pages panel (the NSPanel) has its mirror in the NSPANEL panel.

*/

#include "espurna.h"

#if SCREEN_SUPPORT

#include "espurna_matter.h"
#include "espurna_wifi.h"
#include "json.h"
#include "mqtt.h"
#include "ntp.h"
#include "relay.h"
#include "screen.h"
#include "screen_fonts.h"
#include "screen_icons.h"
#include "sensor.h"
#include "ws.h"

#if WEB_SUPPORT
#include "web.h"
#endif

#include <qrcode.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <memory>
#include <new>

namespace espurna {
namespace screen {
namespace {

static constexpr size_t Lines { 8 };

PROGMEM_STRING(LabelKey, "scrLbl");
PROGMEM_STRING(SourceKey, "scrSrc");

// -----------------------------------------------------------------------------
// The panel, the content, the state
// -----------------------------------------------------------------------------

struct Line {
    espurna::string label;
    espurna::string source;
    espurna::string mqtt_topic;
    espurna::string mqtt_key;
    espurna::string mqtt_value;
};

struct Config {
    ScreenFont font { ScreenFont::Normal };
    uint32_t page_ms { 5000 };
    uint32_t off_ms { 0 };
    uint8_t brightness { 100 };
    uint8_t dim { 0 };
    bool matter { true };
};

constexpr uint32_t QrManualMs { 60000 };
constexpr uint32_t QrRequestMs { 10000 };   // "matter-pair": how long the QR waits for the window to open
constexpr uint32_t QrCheckMs { 500 };
constexpr uint32_t LinesDrawMs { 250 };

struct State {
    ScreenPanel* panel { nullptr };
    const ScreenContent* content { nullptr };
    Config config;
    bool content_sleep { false };      // the content set the blanking (screenSleep())

    Line lines[Lines];
    size_t page { 0 };
    uint32_t page_since { 0 };
    uint32_t last_lines { 0 };
    espurna::string drawn;             // what the lines page shows: drawn again only when it changed

    bool on { true };
    bool dimmed { false };
    uint32_t on_since { 0 };

    bool full { true };                // the next draw: everything
    bool dirty { false };              // the content asked for a draw
    bool drawing { false };
    bool mirror_full { false };        // the WebUI mirror wants the screen in full: drawn for it, not to the panel
    bool mirror_only { false };        // drawing now goes to the mirror only
    espurna::string rows[Lines];       // what each row of the lines shows now (a row is drawn again when it changed)

    // the QR page
    bool qr_shown { false };
    bool qr_manual { false };
    bool qr_dismissed { false };
    bool qr_window_was { false };
    bool qr_refused { false };
    uint32_t qr_since { 0 };
    uint32_t qr_requested { 0 };
    uint32_t qr_check { 0 };
    espurna::string qr_text;
};

State state;

bool pixels() {
    return state.panel && !state.panel->ownPages();
}

// -----------------------------------------------------------------------------
// The WebUI mirror: the drawing commands of a pixel panel
// -----------------------------------------------------------------------------

#if WEB_SUPPORT
namespace mirror {

constexpr size_t Max { 8192 };

bool active { false };
bool overflow { false };
espurna::string out;

void add(const char* format, ...) __attribute__((format(printf, 1, 2)));

void add(const char* format, ...) {
    if (!active || overflow) {
        return;
    }

    char buffer[160];
    va_list args;
    va_start(args, format);
    const int size = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (size <= 0) {
        return;
    }

    if (out.length() + static_cast<size_t>(size) + 1 > Max) {
        overflow = true;
        return;
    }

    out += buffer;
    out += '\n';
}

// a text: after the numbers, to the end of the line (newlines of the text become spaces)
void text(int x, int y, int w, int h, ScreenFont font, uint16_t fore, int32_t back, ScreenAlign align, const char* value) {
    if (!active || overflow) {
        return;
    }

    add("T %d %d %d %d %u %u %ld %u", x, y, w, h, static_cast<unsigned>(font), fore, static_cast<long>(back),
        static_cast<unsigned>(align));
    if (overflow || !out.length()) {
        return;
    }

    out.remove(out.length() - 1);   // the newline of add()
    out += '|';
    for (const char* p = value; *p; ++p) {
        out += ((*p == '\n') || (*p == '\r')) ? ' ' : *p;
    }
    out += '\n';
    if (out.length() > Max) {
        overflow = true;
    }
}

} // namespace mirror
#define MIRROR(...) mirror::add(__VA_ARGS__)
#else
#define MIRROR(...) do {} while (false)
#endif

// -----------------------------------------------------------------------------
// Fonts and icons
// -----------------------------------------------------------------------------

const fonts::Font& font_of(ScreenFont font) {
    switch (font) {
    case ScreenFont::Small:
        return fonts::Small;
    case ScreenFont::Normal:
        break;
    case ScreenFont::Large:
        return fonts::Large;
    case ScreenFont::Big:
        return fonts::Big;
    case ScreenFont::Huge:
        return fonts::Huge;
    }

    return fonts::Normal;
}

const fonts::Glyph* glyph_of(const fonts::Font& font, uint16_t code) {
    size_t low = 0;
    size_t high = font.count;
    while (low < high) {
        const size_t middle = (low + high) / 2;
        const auto value = font.glyphs[middle].code;
        if (value == code) {
            return &font.glyphs[middle];
        }
        if (value < code) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return nullptr;
}

// UTF-8 to code points of the BMP; a broken sequence is skipped byte by byte
template <typename T>
void each_code(const char* text, T&& callback) {
    const auto* p = reinterpret_cast<const uint8_t*>(text);
    while (*p) {
        uint32_t code = *p;
        size_t more = 0;
        if (code < 0x80) {
            more = 0;
        } else if ((code & 0xe0) == 0xc0) {
            code &= 0x1f;
            more = 1;
        } else if ((code & 0xf0) == 0xe0) {
            code &= 0x0f;
            more = 2;
        } else {
            ++p;
            continue;
        }

        ++p;
        bool valid = true;
        for (size_t index = 0; index < more; ++index) {
            if ((*p & 0xc0) != 0x80) {
                valid = false;
                break;
            }
            code = (code << 6) | (*p & 0x3f);
            ++p;
        }

        if (valid) {
            callback(static_cast<uint16_t>(code));
        }
    }
}

int text_width(const fonts::Font& font, const char* text) {
    int out = 0;
    each_code(text, [&](uint16_t code) {
        const auto* glyph = glyph_of(font, code);
        if (glyph) {
            out += glyph->advance;
        }
    });

    return out;
}

uint32_t glyph_offset(const fonts::Glyph& glyph) {
    return static_cast<uint32_t>(glyph.offset_low) | (static_cast<uint32_t>(glyph.offset_high) << 16);
}

// the largest set not larger than the square (96: the weather icons only, the others come from the 48 one)
const icons::Set& icon_set(int size) {
    return (size >= 96) ? icons::Set96 : (size >= 48) ? icons::Set48 : icons::Set24;
}

const uint8_t* icon_bits(const icons::Set& set, uint16_t code) {
    size_t low = 0;
    size_t high = set.count;
    while (low < high) {
        const size_t middle = (low + high) / 2;
        const auto value = set.icons[middle].code;
        if (value == code) {
            const size_t row = (set.size + 7) / 8;
            return set.bits + static_cast<size_t>(set.icons[middle].index) * row * set.size;
        }
        if (value < code) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return nullptr;
}

// -----------------------------------------------------------------------------
// Drawing
// -----------------------------------------------------------------------------

void fill(int x, int y, int w, int h, uint16_t colour) {
    if (!state.panel || state.mirror_only || (w <= 0) || (h <= 0)) {
        return;
    }

    const int width = state.panel->width();
    const int height = state.panel->height();
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    w = std::min(w, width - x);
    h = std::min(h, height - y);
    if ((w <= 0) || (h <= 0)) {
        return;
    }

    state.panel->fill(x, y, w, h, colour);
}

void text(int x, int y, int w, int h, ScreenFont font, uint16_t fore, int32_t back, ScreenAlign align, const char* value) {
    if (!state.panel || !value) {
        return;
    }

#if WEB_SUPPORT
    if (pixels()) {
        mirror::text(x, y, w, h, font, fore, back, align, value);
    }
#endif

    if (state.mirror_only) {
        return;
    }

    if (state.panel->text(x, y, w, h, font, fore, back, align, value)) {
        return;
    }

    // with a background: no clearing of the box first (it flashes on a SPI panel), the background goes around and
    // under the glyphs as they are drawn, the rest of the box after them
    const bool filled = back >= 0;
    const auto background = static_cast<uint16_t>(back);
    int covered = x;

    const auto& data = font_of(font);
    const int width = text_width(data, value);
    int left = x;
    switch (align) {
    case ScreenAlign::Left:
        break;
    case ScreenAlign::Center:
        left = x + (w - width) / 2;
        break;
    case ScreenAlign::Right:
        left = x + w - width;
        break;
    }

    left = std::max(left, x);
    const int top = y + (h - static_cast<int>(data.height)) / 2;
    const int right = x + w;

    each_code(value, [&](uint16_t code) {
        const auto* glyph = glyph_of(data, code);
        if (!glyph) {
            return;
        }

        if (glyph->w && glyph->h) {
            const int gx = left + glyph->x;
            if (gx + glyph->w > right) {
                left = right;   // cut: nothing after it either
                return;
            }
            const int gy = top + glyph->y;
            if (!filled) {
                state.panel->bits(gx, gy, glyph->w, glyph->h, data.bits + glyph_offset(*glyph), fore, ScreenNoBack);
            } else if (gx < covered) {
                // over the glyph before it (a negative bearing): its own pixels only
                state.panel->bits(gx, gy, glyph->w, glyph->h, data.bits + glyph_offset(*glyph), fore, ScreenNoBack);
            } else {
                fill(covered, y, gx - covered, h, background);
                fill(gx, y, glyph->w, gy - y, background);
                fill(gx, gy + glyph->h, glyph->w, (y + h) - (gy + glyph->h), background);
                // the glyph's box with the background (one window of a SPI panel, not a run each)
                state.panel->bits(gx, gy, glyph->w, glyph->h, data.bits + glyph_offset(*glyph), fore, back);
            }
            covered = std::max(covered, gx + glyph->w);
        }

        left += glyph->advance;
    });

    if (filled && (covered < x + w)) {
        fill(covered, y, x + w - covered, h, background);
    }
}

void frame(int x, int y, int w, int h, int t, uint16_t colour) {
    fill(x, y, w, t, colour);
    fill(x, y + h - t, w, t, colour);
    fill(x, y + t, t, h - 2 * t, colour);
    fill(x + w - t, y + t, t, h - 2 * t, colour);
}

// -----------------------------------------------------------------------------
// The lines
// -----------------------------------------------------------------------------

ScreenFont line_font(int value) {
    return (value <= 10) ? ScreenFont::Small : (value >= 24) ? ScreenFont::Large : ScreenFont::Normal;
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

size_t line_pitch() {
    return font_of(state.config.font).height;
}

size_t rows() {
    if (!state.panel) {
        return 1;
    }

    return std::max<size_t>(1, static_cast<size_t>(state.panel->height()) / line_pitch());
}

void lines_draw(bool full) {
    size_t used[Lines];
    const auto count = used_lines(used);
    const auto per_page = rows();
    const auto pages = std::max<size_t>(1, (count + per_page - 1) / per_page);
    if (state.page >= pages) {
        state.page = 0;
    }

    // what the page says: drawn again only when it changed
    espurna::string summary;
    espurna::string values[Lines];
    for (size_t row = 0; (row < per_page) && (row < Lines); ++row) {
        const auto position = state.page * per_page + row;
        if (position >= count) {
            break;
        }

        const auto& line = state.lines[used[position]];
        values[row] = value_of(line);
        summary += line.label;
        summary += '\t';
        summary += values[row];
        summary += '\n';
    }

    if (!count) {
        summary = systemHostname();
    }

    summary += espurna::string(static_cast<unsigned long>(state.page));
    if (!full && summary.equals(state.drawn)) {
        return;
    }

    state.drawn = summary;

    const int width = state.panel->width();
    const int height = state.panel->height();
    const int pitch = static_cast<int>(line_pitch());
    const auto font = state.config.font;
    const int margin = (width >= 200) ? 6 : 0;

    if (full || !count) {
        fill(0, 0, width, height, ScreenBlack);
        MIRROR("F 0 0 %d %d 0", width, height);
        for (auto& row : state.rows) {
            row = espurna::string();
        }
    }

    if (!count) {
        // nothing set yet: the name of the board, so the screen shows it is alive
        const auto host = systemHostname();
        text(0, 0, width, pitch, font, ScreenWhite, ScreenNoBack, ScreenAlign::Center, host.c_str());
    }

    for (size_t row = 0; (row < per_page) && (row < Lines); ++row) {
        const auto position = state.page * per_page + row;
        if (position >= count) {
            break;
        }

        const auto& line = state.lines[used[position]];
        const int y = static_cast<int>(row) * pitch;

        // drawn again only when it shows something else (a value, another page)
        auto shown = line.label + '\t' + values[row];
        if (shown.equals(state.rows[row])) {
            continue;
        }
        state.rows[row] = std::move(shown);
        fill(0, y, width, pitch, ScreenBlack);
        MIRROR("F 0 %d %d %d 0", y, width, pitch);

        if (line.label.length()) {
            text(margin, y, width - 2 * margin, pitch, font, ScreenWhite, ScreenNoBack, ScreenAlign::Left,
                line.label.c_str());
            const int label = text_width(font_of(font), line.label.c_str()) + 4;
            text(margin + label, y, width - 2 * margin - label, pitch, font, ScreenWhite, ScreenNoBack,
                ScreenAlign::Right, values[row].c_str());
        } else {
            text(margin, y, width - 2 * margin, pitch, font, ScreenWhite, ScreenNoBack, ScreenAlign::Left,
                values[row].c_str());
        }
    }

    // the rows under the last line of a shorter page
    for (size_t row = std::min(per_page, Lines); row-- > 0;) {
        const auto position = state.page * per_page + row;
        if ((position < count) || !state.rows[row].length()) {
            continue;
        }
        state.rows[row] = espurna::string();
        const int y = static_cast<int>(row) * pitch;
        fill(0, y, width, pitch, ScreenBlack);
        MIRROR("F 0 %d %d %d 0", y, width, pitch);
    }

    if (pages > 1) {
        // the page marks, one dot per page in the right bottom corner
        for (size_t page = 0; page < pages; ++page) {
            const int x = width - 1 - static_cast<int>((pages - 1 - page) * 3);
            const int y = height - 1;
            fill(x, y, 1, 1, ScreenWhite);
            if (page == state.page) {
                fill(x, y - 1, 1, 1, ScreenWhite);
            }
            MIRROR("F %d %d 1 %d 65535", x, (page == state.page) ? y - 1 : y, (page == state.page) ? 2 : 1);
        }
    }
}

// -----------------------------------------------------------------------------
// The QR page
// -----------------------------------------------------------------------------

// ECC low, versions 1..5: a 22 character MT: payload is version 1 (21 x 21)
constexpr uint8_t QrVersionMax { 5 };
constexpr uint16_t QrAlphanumericLow[QrVersionMax] { 25, 47, 77, 114, 154 };
constexpr uint16_t QrBytesLow[QrVersionMax] { 17, 32, 53, 78, 106 };
constexpr int QrSizeMax { 17 + 4 * QrVersionMax };
constexpr size_t QrBufferSize { ((QrSizeMax * QrSizeMax) + 7) / 8 };
constexpr size_t QrRunsMax { (QrSizeMax + 1) / 2 };

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

// dark modules as boxes: a horizontal run is one, the same columns in the next row make it taller (a Nextion draws a
// box per command, a pixel panel gets fewer windows)
struct Run {
    uint8_t x;
    uint8_t w;
    uint8_t y;
    uint8_t h;
};

void qr_modules(QRCode& code, int left, int top, int module) {
    Run open[QrRunsMax];
    size_t open_count { 0 };

    const int size = code.size;
    for (int y = 0; y <= size; ++y) {
        Run next[QrRunsMax];
        size_t next_count { 0 };
        bool taken[QrRunsMax] {};

        for (int x = 0; (y < size) && (x < size);) {
            if (!qrcode_getModule(&code, x, y)) {
                ++x;
                continue;
            }

            const int start = x;
            while ((x < size) && qrcode_getModule(&code, x, y)) {
                ++x;
            }

            Run run { static_cast<uint8_t>(start), static_cast<uint8_t>(x - start), static_cast<uint8_t>(y), 1 };
            for (size_t index = 0; index < open_count; ++index) {
                if (!taken[index] && (open[index].x == run.x) && (open[index].w == run.w)) {
                    taken[index] = true;
                    run.y = open[index].y;
                    run.h = open[index].h + 1;
                    break;
                }
            }

            if (next_count < QrRunsMax) {
                next[next_count++] = run;
            }
        }

        for (size_t index = 0; index < open_count; ++index) {
            if (!taken[index]) {
                const auto& run = open[index];
                const int rx = left + run.x * module;
                const int ry = top + run.y * module;
                fill(rx, ry, run.w * module, run.h * module, ScreenBlack);
                MIRROR("F %d %d %d %d 0", rx, ry, run.w * module, run.h * module);
            }
        }

        std::copy(next, next + next_count, open);
        open_count = next_count;
    }
}

// the code on a white screen, as large as it takes it: a wide screen (twice as wide as high: the small OLEDs) has the
// lines of text on the right of it, the others one line under it. false: the text does not fit a QR code up to 5
bool qr_draw(const char* payload, const char* const* lines, size_t count) {
    const auto version = qr_version(payload);
    uint8_t buffer[QrBufferSize];
    QRCode code;
    if (!version || (qrcode_initText(&code, buffer, version, ECC_LOW, payload) != 0)) {
        return false;
    }

    const int width = state.panel->width();
    const int height = state.panel->height();
    const bool wide = width >= 2 * height;
    const auto font = (height >= 160) ? ScreenFont::Normal : ScreenFont::Small;
    const int line = static_cast<int>(font_of(font).height);
    const int under = wide ? 0 : (line + 4);

    // a quiet zone of 4 modules where the screen takes it, else 2
    int quiet = 4;
    int module = (height - under) / (code.size + 2 * quiet);
    if (module < 1) {
        quiet = 2;
        module = std::max(1, (height - under) / (code.size + 2 * quiet));
    }

    const int side = (code.size + 2 * quiet) * module;
    const int left = wide ? 0 : (width - side) / 2;
    const int top = (height - under - side) / 2;

    fill(0, 0, width, height, ScreenWhite);
    MIRROR("F 0 0 %d %d 65535", width, height);
    qr_modules(code, left + quiet * module, top + quiet * module, module);
    std::memset(buffer, 0, sizeof(buffer));

    if (wide) {
        const int x = side + 4;
        for (size_t index = 0; index < count; ++index) {
            text(x, static_cast<int>(index) * line, width - x, line, font, ScreenBlack, ScreenNoBack,
                ScreenAlign::Left, lines[index]);
        }
    } else {
        espurna::string joined;
        for (size_t index = 0; index < count; ++index) {
            if (index) {
                joined += ' ';
            }
            joined += lines[index];
        }
        text(0, height - under, width, line, font, ScreenBlack, ScreenNoBack, ScreenAlign::Center, joined.c_str());
        joined.clear();
    }

    return true;
}

void qr_page() {
    bool drawn = false;
    if (state.qr_text.length()) {
        const char* lines[] { "QR" };
        drawn = qr_draw(state.qr_text.c_str(), lines, 1);
    } else {
        char payload[32] {};
        char code[24] {};
        if (matterPairingCodes(payload, sizeof(payload), code, sizeof(code))) {
            // "34970112332" -> "3497-011" "2332", as the controller apps print it
            char first[12] {};
            char second[12] {};
            if (std::strlen(code) == 11) {
                snprintf_P(first, sizeof(first), PSTR("%.4s-%.3s"), code, code + 4);
                snprintf_P(second, sizeof(second), PSTR("%.4s"), code + 7);
            } else {
                snprintf_P(first, sizeof(first), PSTR("%.10s"), code);
                snprintf_P(second, sizeof(second), PSTR("%.10s"), (std::strlen(code) > 10) ? code + 10 : "");
            }

            const bool wide = state.panel->width() >= 2 * state.panel->height();
            const char* lines[] { "Matter", first, second };
            if (wide) {
                drawn = qr_draw(payload, lines, std::size(lines));
            } else {
                char joined[24] {};
                snprintf_P(joined, sizeof(joined), PSTR("%s-%s"), first, second);
                const char* one[] { "Matter:", joined };
                drawn = qr_draw(payload, one, std::size(one));
                std::memset(joined, 0, sizeof(joined));
            }

            std::memset(payload, 0, sizeof(payload));
            std::memset(code, 0, sizeof(code));
            std::memset(first, 0, sizeof(first));
            std::memset(second, 0, sizeof(second));
        }
    }

    if (!drawn) {
        const int width = state.panel->width();
        const int height = state.panel->height();
        fill(0, 0, width, height, ScreenWhite);
        MIRROR("F 0 0 %d %d 65535", width, height);
        text(0, 0, width, height, ScreenFont::Small, ScreenBlack, ScreenNoBack, ScreenAlign::Center,
            state.qr_text.length() ? "QR: too long" : "Matter: no pairing codes");
    }
}

bool qr_wanted(uint32_t now) {
    if (state.qr_manual && (now - state.qr_since >= QrManualMs)) {
        state.qr_manual = false;
        state.qr_text = espurna::string();
    }

    const bool window = matterWindowOpen();
    if (window && !state.qr_window_was) {
        state.qr_dismissed = false;
    }
    state.qr_window_was = window;

    // the tile's window: open now, or it did not open in time
    if (state.qr_requested && (window || (now - state.qr_requested >= QrRequestMs))) {
        if (!window) {
            DEBUG_MSG_P(PSTR("[SCREEN] The Matter pairing window did not open in %u s\n"),
                static_cast<unsigned>(QrRequestMs / 1000));
        }
        state.qr_requested = 0;
    }

    return state.qr_manual || (state.config.matter && window && !state.qr_dismissed) || state.qr_requested;
}

// -----------------------------------------------------------------------------
// On, off, dimmed
// -----------------------------------------------------------------------------

void power(bool on) {
    if (!state.panel) {
        return;
    }

    if (on) {
        state.on_since = espurna::time::ms();
    }

    if ((state.on == on) && !(on && state.dimmed)) {
        return;
    }

    const bool was_off = !state.on;
    state.on = on;
    state.dimmed = false;
    if (on) {
        if (was_off) {
            state.panel->power(true);
            state.full = true;
        }
        state.panel->brightness(state.config.brightness);
    } else {
        state.panel->power(false);
    }

    MIRROR("P %d", on ? 1 : 0);
}

void dim() {
    if (!state.panel || !state.on || state.dimmed) {
        return;
    }

    if (!state.config.dim) {
        power(false);
        return;
    }

    state.dimmed = true;
    state.panel->brightness(state.config.dim);
}

// -----------------------------------------------------------------------------
// The loop
// -----------------------------------------------------------------------------

void frame_end() {
    if (state.panel && !state.mirror_only) {
        state.panel->flush();
    }
}

void draw_now(bool full) {
    if (!state.panel || state.drawing) {
        return;
    }

    state.drawing = true;
    if (full) {
        MIRROR("Z %d %d %d %d", state.panel->width(), state.panel->height(), state.panel->colour() ? 1 : 0,
            state.on ? 1 : 0);
    }

    if (state.qr_shown) {
        qr_page();
    } else if (state.content) {
        state.content->draw(full);
    } else {
        lines_draw(full);
    }

    state.drawing = false;
    frame_end();
}

void qr_loop(uint32_t now) {
    if (now - state.qr_check < QrCheckMs) {
        return;
    }
    state.qr_check = now;

    const bool wanted = qr_wanted(now);
    if (!wanted) {
        if (state.qr_shown) {
            state.qr_shown = false;
            state.panel->hide();
            state.full = true;
            DEBUG_MSG_P(PSTR("[SCREEN] QR page gone\n"));
        }
        return;
    }

    if (state.qr_shown && state.panel->shown()) {
        return;
    }

    // a blanked screen wakes up for it
    power(true);

    // shown before show(): an own-pages panel draws it from show() already (screenPanelDraw())
    state.qr_shown = true;
    if (!state.panel->show()) {
        state.qr_shown = false;
        if (!state.qr_refused) {
            state.qr_refused = true;
            DEBUG_MSG_P(PSTR("[SCREEN] The panel %s does not show the QR page now\n"), state.panel->name());
        }
        return;
    }

    state.qr_refused = false;
    DEBUG_MSG_P(PSTR("[SCREEN] QR page shown\n"));
    if (pixels()) {
        state.full = true;
    }
}

void loop() {
    if (!state.panel) {
        return;
    }

    const uint32_t now = espurna::time::ms();
    qr_loop(now);

    if (state.config.off_ms && state.on && !state.dimmed && !state.qr_shown
        && (now - state.on_since > state.config.off_ms))
    {
        dim();
    }

    // the own-pages panel draws its pages itself, the screen only when it asks (screenPanelDraw())
    if (!pixels()) {
        return;
    }

    if (!state.on) {
        return;
    }

    if (!state.content && !state.qr_shown) {
        if (now - state.page_since > state.config.page_ms) {
            state.page_since = now;
            ++state.page;
            state.dirty = true;
        }

        if (now - state.last_lines >= LinesDrawMs) {
            state.last_lines = now;
            state.dirty = true;
        }
    }

    if (state.full) {
        state.full = false;
        state.dirty = false;
        state.mirror_full = false;
        draw_now(true);
        return;
    }

    // what changed first (to the panel), then the whole screen once more for the mirror alone
    if (state.mirror_full) {
        if (state.dirty) {
            state.dirty = false;
            draw_now(false);
        }
        state.mirror_full = false;
        state.mirror_only = true;
        draw_now(true);
        state.mirror_only = false;
        return;
    }

    if (state.dirty) {
        state.dirty = false;
        if (!state.qr_shown) {
            draw_now(false);
        }
    }
}

// -----------------------------------------------------------------------------
// Settings
// -----------------------------------------------------------------------------

// the lines of the oled module become the screen's, once
void migrate_oled() {
    struct Pair {
        const char* from;
        const char* to;
    };

    static constexpr Pair Plain[] {
        {"oledFont", "scrFont"},
        {"oledPage", "scrPage"},
        {"oledOff", "scrOff"},
        {"oledMatter", "scrMatter"},
    };

    for (const auto& pair : Plain) {
        const auto value = getSetting(pair.from);
        if (value.length()) {
            if (!hasSetting(pair.to)) {
                setSetting(pair.to, value);
            }
            delSetting(pair.from);
        }
    }

    // 0..255 of the controller to %
    const auto bright = getSetting("oledBright");
    if (bright.length()) {
        const auto value = bright.toInt();
        if ((value >= 0) && !hasSetting("scrBright")) {
            setSetting("scrBright", static_cast<int>(std::min(100l, (static_cast<long>(value) * 100l + 254l) / 255l)));
        }
        delSetting("oledBright");
    }

    for (size_t index = 0; index < Lines; ++index) {
        const auto label = getSetting({"oledLbl", index});
        if (label.length()) {
            if (!hasSetting({LabelKey, index})) {
                setSetting({LabelKey, index}, label);
            }
            delSetting({"oledLbl", index});
        }

        const auto source = getSetting({"oledSrc", index});
        if (source.length()) {
            if (!hasSetting({SourceKey, index})) {
                setSetting({SourceKey, index}, source);
            }
            delSetting({"oledSrc", index});
        }
    }
}

void configure() {
    const auto font = state.config.font;
    const auto page_ms = state.config.page_ms;
    espurna::string before;
    for (const auto& line : state.lines) {
        before += line.label;
        before += '\t';
        before += line.source;
        before += '\n';
    }

    state.config.font = line_font(getSetting("scrFont", 16));
    state.config.page_ms = std::max(1ul, getSetting("scrPage", 5ul)) * 1000ul;
    state.config.matter = getSetting("scrMatter", true);
    if (!state.content_sleep) {
        state.config.off_ms = getSetting("scrOff", 0ul) * 1000ul;
        state.config.brightness = static_cast<uint8_t>(std::min(100, std::max(1, getSetting("scrBright", 100))));
        state.config.dim = 0;
    }

    read_lines();

    espurna::string after;
    for (const auto& line : state.lines) {
        after += line.label;
        after += '\t';
        after += line.source;
        after += '\n';
    }

    if (!after.equals(before) || (font != state.config.font) || (page_ms != state.config.page_ms)) {
        state.page = 0;
        state.page_since = espurna::time::ms();
        state.drawn = espurna::string();
        if (!state.content) {
            state.full = true;
        }
    }

    if (state.panel && state.on && !state.dimmed) {
        state.panel->brightness(state.config.brightness);
    }
}

// -----------------------------------------------------------------------------
// MQTT: the lines of mqtt:<topic>
// -----------------------------------------------------------------------------

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

// -----------------------------------------------------------------------------
// Terminal
// -----------------------------------------------------------------------------

#if TERMINAL_SUPPORT
PROGMEM_STRING(Screen, "SCREEN");

void terminal_screen(::terminal::CommandContext&& ctx) {
    if (!state.panel) {
        terminalError(ctx, STRING_VIEW("no panel (the oled, lcd or nspanel module, its settings)"));
        return;
    }

    ctx.output.printf_P(PSTR("panel %s %dx%d %s%s, %s%s, drawn by %s, page %zu, QR %s\n"),
        state.panel->name(), state.panel->width(), state.panel->height(),
        state.panel->colour() ? "colour" : "1 bit", state.panel->ownPages() ? " with its own pages" : "",
        state.on ? "on" : "off", state.dimmed ? " (dimmed)" : "",
        state.content ? state.content->name : "the lines", state.page,
        state.qr_shown ? "shown" : "none");
    ctx.output.printf_P(PSTR("off after %u s, brightness %u %%, dim %u %%%s\n"),
        static_cast<unsigned>(state.config.off_ms / 1000), state.config.brightness, state.config.dim,
        state.content_sleep ? " (set by the content)" : "");
    terminalOK(ctx);
}

PROGMEM_STRING(ScreenQr, "SCREEN.QR");

// SCREEN.QR - the Matter pairing code for a minute (whatever the window), SCREEN.QR <text> - a QR code of the text,
// SCREEN.QR off - the page goes
void terminal_qr(::terminal::CommandContext&& ctx) {
    if (!state.panel) {
        terminalError(ctx, STRING_VIEW("no panel"));
        return;
    }

    if ((ctx.argv.size() == 2) && ctx.argv[1].equalsIgnoreCase(STRING_VIEW("off"))) {
        state.qr_manual = false;
        state.qr_dismissed = true;
        state.qr_requested = 0;
        state.qr_text = espurna::string();
        state.qr_check = espurna::time::ms() - QrCheckMs;
        terminalOK(ctx);
        return;
    }

    state.qr_manual = true;
    state.qr_dismissed = false;
    state.qr_since = espurna::time::ms();
    state.qr_check = state.qr_since - QrCheckMs;
    state.qr_text = espurna::string();
    for (size_t index = 1; index < ctx.argv.size(); ++index) {
        if (index > 1) {
            state.qr_text += ' ';
        }
        state.qr_text += ctx.argv[index];
    }

    // a new text on a QR page already shown: shown again
    state.qr_shown = false;
    terminalOK(ctx);
}

PROGMEM_STRING(ScreenKeyCommand, "SCREEN.KEY");

// SCREEN.KEY prev | next | enter | back (a button with the terminal command action, the WebUI)
void terminal_key(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() != 2) {
        terminalError(ctx, STRING_VIEW("SCREEN.KEY prev | next | enter | back"));
        return;
    }

    const auto& name = ctx.argv[1];
    ScreenKey key;
    if (name.equalsIgnoreCase(STRING_VIEW("prev"))) {
        key = ScreenKey::Prev;
    } else if (name.equalsIgnoreCase(STRING_VIEW("next"))) {
        key = ScreenKey::Next;
    } else if (name.equalsIgnoreCase(STRING_VIEW("enter")) || name.equalsIgnoreCase(STRING_VIEW("ok"))) {
        key = ScreenKey::Enter;
    } else if (name.equalsIgnoreCase(STRING_VIEW("back"))) {
        key = ScreenKey::Back;
    } else {
        terminalError(ctx, STRING_VIEW("SCREEN.KEY prev | next | enter | back"));
        return;
    }

    screenKey(key);
    terminalOK(ctx);
}

static constexpr ::terminal::Command Commands[] PROGMEM {
    {Screen, terminal_screen},
    {ScreenQr, terminal_qr},
    {ScreenKeyCommand, terminal_key},
};
#endif

// -----------------------------------------------------------------------------
// WebUI
// -----------------------------------------------------------------------------

#if WEB_SUPPORT
void on_visible(espurna::json::Writer& root) {
    wsPayloadModule(root, STRING_VIEW("screen"));
}

void on_connected(espurna::json::Writer& root) {
    root.field(STRING_VIEW("scrFont"), getSetting("scrFont", 16));
    root.field(STRING_VIEW("scrPage"), getSetting("scrPage", 5ul));
    root.field(STRING_VIEW("scrOff"), getSetting("scrOff", 0ul));
    root.field(STRING_VIEW("scrBright"), getSetting("scrBright", 100));
    root.field(STRING_VIEW("scrMatter"), getSetting("scrMatter", true));

    for (size_t index = 0; index < Lines; ++index) {
        const auto label = espurna::settings::Key(LabelKey, index);
        const auto source = espurna::settings::Key(SourceKey, index);
        root.field(StringView(label.c_str(), label.length()), state.lines[index].label);
        root.field(StringView(source.c_str(), source.length()), state.lines[index].source);
    }

    // the browser gets the screen in full (drawn for it alone: the panel keeps what it shows)
    mirror::active = pixels();
    mirror::overflow = false;
    mirror::out = espurna::string();
    state.mirror_full = true;
}

bool on_key_check(espurna::StringView key, espurna::json::Value) {
    return key.startsWith(STRING_VIEW("scr"));
}

void on_action(uint32_t, const char* action, espurna::json::Value data) {
    if (std::strcmp(action, "screenKey") != 0) {
        return;
    }

    const auto name = data[STRING_VIEW("key")].asString();
    if (name.equals(STRING_VIEW("prev"))) {
        screenKey(ScreenKey::Prev);
    } else if (name.equals(STRING_VIEW("next"))) {
        screenKey(ScreenKey::Next);
    } else if (name.equals(STRING_VIEW("enter"))) {
        screenKey(ScreenKey::Enter);
    } else if (name.equals(STRING_VIEW("back"))) {
        screenKey(ScreenKey::Back);
    }
}

// GET /screen.raw: "<width> <height>"; ?y=<row>&h=<rows> (at most RawRowsMax): those rows as the panel shows them,
// RGB565 big-endian, read from the panel itself (not drawn again): the pictures of the documentation
constexpr int RawRowsMax { 16 };

void on_raw(WebRequest& request) {
    if (!webAuthenticate(request)) {
        webRequestAuthentication(request);
        return;
    }

    if (!state.panel) {
        webSend(request, 404, "text/plain", espurna::string("no panel"));
        return;
    }

    const int width = state.panel->width();
    const int height = state.panel->height();
    const auto row = webParam(request, "y");
    if (!row.length()) {
        webSend(request, 200, "text/plain", espurna::string(width) + ' ' + espurna::string(height));
        return;
    }

    const int y = row.toInt();
    const int rows = std::min(RawRowsMax, std::max(1, static_cast<int>(webParam(request, "h").toInt())));
    if ((y < 0) || (y + rows > height)) {
        webSend(request, 400, "text/plain", espurna::string("rows out of the panel"));
        return;
    }

    std::unique_ptr<uint16_t[]> pixels(new (std::nothrow) uint16_t[static_cast<size_t>(width) * rows]);
    if (!pixels || !state.panel->read(0, y, width, rows, pixels.get())) {
        webSend(request, pixels ? 501 : 503, "text/plain", espurna::string(pixels ? "the panel cannot be read" : "no memory"));
        return;
    }

    espurna::string out;
    out.reserve(static_cast<size_t>(width) * rows * 2);
    for (int index = 0; index < width * rows; ++index) {
        out += static_cast<char>(pixels[index] >> 8);
        out += static_cast<char>(pixels[index] & 0xff);
    }
    pixels.reset();
    webSend(request, 200, "application/octet-stream", std::move(out));
}

void mirror_loop() {
    if (!mirror::active) {
        return;
    }

    if (!wsConnected() || !pixels()) {
        mirror::active = false;
        mirror::out = espurna::string();
        return;
    }

    if (mirror::overflow) {
        mirror::overflow = false;
        mirror::out = espurna::string();
        state.mirror_full = true;
        return;
    }

    if (!mirror::out.length()) {
        return;
    }

    auto out = std::move(mirror::out);
    mirror::out = espurna::string();
    wsPost([out](espurna::json::Writer& root) {
        root.field(STRING_VIEW("screenMirror"), out);
    });
}
#endif

} // namespace
} // namespace screen
} // namespace espurna

// -----------------------------------------------------------------------------
// The panels' default
// -----------------------------------------------------------------------------

void ScreenPanel::bits(int x, int y, int w, int h, const uint8_t* rows, uint16_t fore, int32_t back) {
    using espurna::screen::fill;
    const int stride = (w + 7) / 8;
    for (int row = 0; row < h; ++row) {
        const uint8_t* line = rows + row * stride;
        int column = 0;
        while (column < w) {
            const bool set = (line[column / 8] >> (7 - (column % 8))) & 1;
            int end = column + 1;
            while ((end < w) && (((line[end / 8] >> (7 - (end % 8))) & 1) == set)) {
                ++end;
            }

            if (set) {
                fill(x + column, y + row, end - column, 1, fore);
            } else if (back >= 0) {
                fill(x + column, y + row, end - column, 1, static_cast<uint16_t>(back));
            }

            column = end;
        }
    }
}

// -----------------------------------------------------------------------------
// The API
// -----------------------------------------------------------------------------

bool screenPanel(ScreenPanel* panel) {
    using namespace espurna::screen;
    if (panel && state.panel && (panel != state.panel)) {
        DEBUG_MSG_P(PSTR("[SCREEN] %s refused: the screen has %s\n"), panel->name(), state.panel->name());
        return false;
    }

    state.panel = panel;
    state.qr_shown = false;
    state.full = true;
    state.drawn = espurna::string();
    if (!panel) {
        DEBUG_MSG_P(PSTR("[SCREEN] No panel\n"));
        return true;
    }

    DEBUG_MSG_P(PSTR("[SCREEN] Panel %s %dx%d%s\n"), panel->name(), panel->width(), panel->height(),
        panel->ownPages() ? ", its own pages" : "");
    state.on = false;
    power(true);
    return true;
}

void screenPanelRedraw() {
    espurna::screen::state.full = true;
}

void screenPanelDraw() {
    using namespace espurna::screen;
    if (!state.panel || !state.qr_shown || state.drawing) {
        return;
    }

    state.drawing = true;
    qr_page();
    state.drawing = false;
    frame_end();
}

void screenPanelClosed() {
    using namespace espurna::screen;
    state.qr_shown = false;
    state.qr_dismissed = true;
    state.qr_manual = false;
    state.qr_requested = 0;
    DEBUG_MSG_P(PSTR("[SCREEN] QR page closed\n"));
}

void screenContent(const ScreenContent* content) {
    using namespace espurna::screen;
    state.content = content;
    if (!content) {
        state.content_sleep = false;
        configure();
    }
    state.full = true;
}

void screenInvalidate() {
    espurna::screen::state.dirty = true;
}

bool screenReady() {
    return espurna::screen::state.panel != nullptr;
}

const char* screenPanelName() {
    using namespace espurna::screen;
    return state.panel ? state.panel->name() : "";
}

int screenWidth() {
    using namespace espurna::screen;
    return state.panel ? state.panel->width() : 0;
}

int screenHeight() {
    using namespace espurna::screen;
    return state.panel ? state.panel->height() : 0;
}

bool screenColour() {
    using namespace espurna::screen;
    return state.panel && state.panel->colour();
}

bool screenOn() {
    using namespace espurna::screen;
    return state.panel && state.on;
}

bool screenQrShown() {
    return espurna::screen::state.qr_shown;
}

bool screenOwnPages() {
    using namespace espurna::screen;
    return state.panel && state.panel->ownPages();
}

void screenFill(int x, int y, int w, int h, uint16_t colour) {
    using namespace espurna::screen;
    fill(x, y, w, h, colour);
    MIRROR("F %d %d %d %d %u", x, y, w, h, colour);
}

void screenFrame(int x, int y, int w, int h, int thickness, uint16_t colour) {
    using namespace espurna::screen;
    frame(x, y, w, h, thickness, colour);
    MIRROR("R %d %d %d %d %d %u", x, y, w, h, thickness, colour);
}

void screenLine(int x0, int y0, int x1, int y1, uint16_t colour) {
    using namespace espurna::screen;
    MIRROR("L %d %d %d %d %u", x0, y0, x1, y1, colour);

    // straight ones as one box
    if ((x0 == x1) || (y0 == y1)) {
        fill(std::min(x0, x1), std::min(y0, y1), std::abs(x1 - x0) + 1, std::abs(y1 - y0) + 1, colour);
        return;
    }

    const int dx = std::abs(x1 - x0);
    const int dy = -std::abs(y1 - y0);
    const int sx = (x0 < x1) ? 1 : -1;
    const int sy = (y0 < y1) ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        fill(x0, y0, 1, 1, colour);
        if ((x0 == x1) && (y0 == y1)) {
            break;
        }
        const int twice = 2 * error;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void screenCircle(int x, int y, int r, uint16_t colour, bool filled) {
    using namespace espurna::screen;
    MIRROR("C %d %d %d %u %d", x, y, r, colour, filled ? 1 : 0);

    // by rows: a filled one is a box per row, an outline the two ends of the row (and the steps between them)
    int previous = -1;
    for (int dy = r; dy >= 0; --dy) {
        const int dx = static_cast<int>(std::lround(std::sqrt(static_cast<double>(r * r - dy * dy))));
        if (filled) {
            fill(x - dx, y - dy, 2 * dx + 1, 1, colour);
            if (dy) {
                fill(x - dx, y + dy, 2 * dx + 1, 1, colour);
            }
            continue;
        }

        const int from = (previous < 0) ? dx : std::min(dx, previous + 1);
        const int run = std::max(1, dx - from + 1);
        fill(x - dx, y - dy, run, 1, colour);
        fill(x + dx - run + 1, y - dy, run, 1, colour);
        if (dy) {
            fill(x - dx, y + dy, run, 1, colour);
            fill(x + dx - run + 1, y + dy, run, 1, colour);
        }
        previous = dx;
    }
}

void screenText(int x, int y, int w, int h, ScreenFont font, uint16_t fore, int32_t back, ScreenAlign align,
    const char* value)
{
    espurna::screen::text(x, y, w, h, font, fore, back, align, value);
}

int screenTextWidth(ScreenFont font, const char* text) {
    using namespace espurna::screen;
    return text ? text_width(font_of(font), text) : 0;
}

int screenFontHeight(ScreenFont font) {
    using namespace espurna::screen;
    return font_of(font).height;
}

bool screenIcon(int x, int y, int size, uint16_t code, uint16_t fore, int32_t back) {
    using namespace espurna::screen;
    if (!state.panel) {
        return false;
    }

    const auto* set = &icon_set(size);
    const auto* bits = icon_bits(*set, code);
    if (!bits && (set->size > 48)) {
        set = &icons::Set48;
        bits = icon_bits(*set, code);
    }
    if (!bits) {
        return false;
    }

    MIRROR("I %d %d %d %u %u %ld", x, y, size, code, fore, static_cast<long>(back));
    if (state.mirror_only) {
        return true;
    }

    // the bitmap's square with its background, the margin around it (a smaller set) filled, nothing cleared first
    const int offset = (size - set->size) / 2;
    if ((back >= 0) && (offset > 0)) {
        const auto colour = static_cast<uint16_t>(back);
        fill(x, y, size, offset, colour);
        fill(x, y + offset + set->size, size, size - offset - set->size, colour);
        fill(x, y + offset, offset, set->size, colour);
        fill(x + offset + set->size, y + offset, size - offset - set->size, set->size, colour);
    }
    state.panel->bits(x + offset, y + offset, set->size, set->size, bits, fore, back);
    return true;
}

void screenSleep(uint32_t seconds, uint8_t brightness, uint8_t dim) {
    using namespace espurna::screen;
    state.content_sleep = true;
    state.config.off_ms = seconds * 1000ul;
    state.config.brightness = std::max<uint8_t>(1, std::min<uint8_t>(100, brightness));
    state.config.dim = std::min<uint8_t>(100, dim);
    if (state.panel && state.on && !state.dimmed) {
        state.panel->brightness(state.config.brightness);
    }
}

void screenKey(ScreenKey key) {
    using namespace espurna::screen;
    if (!state.panel) {
        return;
    }

    // a blanked or dimmed screen only wakes up
    if (!state.on || state.dimmed) {
        power(true);
        return;
    }

    state.on_since = espurna::time::ms();

    if (state.qr_shown) {
        screenPanelClosed();
        state.panel->hide();
        state.full = true;
        return;
    }

    if (state.content) {
        state.content->key(key);
        return;
    }

    // the lines: the pages
    switch (key) {
    case ScreenKey::Prev:
        state.page = state.page ? state.page - 1 : 0;
        break;
    case ScreenKey::Next:
    case ScreenKey::Enter:
        ++state.page;
        break;
    case ScreenKey::Back:
        state.page = 0;
        break;
    }

    state.page_since = espurna::time::ms();
    state.dirty = true;
}

bool screenMatterPair() {
    using namespace espurna::screen;
    if (!matterWindowStart()) {
        DEBUG_MSG_P(PSTR("[SCREEN] Matter pairing: Matter is not started\n"));
        return false;
    }

    state.qr_requested = espurna::time::ms() | 1;
    state.qr_dismissed = false;
    state.qr_check = espurna::time::ms() - QrCheckMs;
    DEBUG_MSG_P(PSTR("[SCREEN] Matter pairing window asked for\n"));
    return true;
}

void displayOn() {
    using namespace espurna::screen;
    power(true);
}

void screenSetup() {
    using namespace espurna::screen;

    migrate_oled();
    configure();

    mqttRegister(on_mqtt);
#if TERMINAL_SUPPORT
    espurna::terminal::add(Commands);
#endif
#if WEB_SUPPORT
    wsRegister()
        .onVisible(on_visible)
        .onConnected(on_connected)
        .onAction(on_action)
        .onKeyCheck(on_key_check);
    espurnaRegisterLoop(mirror_loop);
    webServer().on("/screen.raw", WebMethod::Get, on_raw);
#endif

    espurnaRegisterLoop(loop);
    espurnaRegisterReload(configure);

    // the tile pages of /nspanel.json (screen_pages.cpp), and their tile "matter-pair": the QR page here
    pagesSetup();
    pagesAction("matter-pair", []() {
        screenMatterPair();
    });
}

#endif // SCREEN_SUPPORT
