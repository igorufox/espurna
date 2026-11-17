/*

NSPANEL QR (part of the nspanel module, the owner 2026-11-17: not a module of its own): the Matter pairing QR code on
the NSPanel display

The matter module is called through the head (modules_api.cpp, the provides column of matter in config/head.h): the
nspanel module does not need it; without it there is no window and no codes, the QR never shows and "matter-pair"
logs that Matter is not started.

The matter module gives the codes (matterPairingCodes()) and whether the pairing window is open (matterWindowOpen()),
the OHpanel driver of the nspanel module the screen (nspanelScreenShow(), nspanel.h). The QR matrix (ricmoo/QRCode,
libraries.ini) exists only while it is drawn, on the stack: Nextion `fill` rectangles, a horizontal run of dark
modules is one, the same run in the rows below grows it down; a white screen with the quiet zone of 4 modules, the
module as large as the 480 x 320 screen takes with one text line under it (version 1, 21 x 21: 10 px).

- the pairing window opens: the QR shows, and goes when the window closes (also once a controller added the device)
- NSPANEL.QR shows it whatever the window, NSPANEL.QR off takes it away
- a touch takes it away, until the next window
- the tile action "matter-pair" (nspanelAction(), nspanel.h; /nspanel.json: {"name": "Matter", "icon": "plus",
  "action": "matter-pair"}) opens the basic pairing window through the matter module (matterWindowStart(), the same as
  MATTER.WINDOW) and shows the QR at once; it goes as above. The window not open in 10 s: the QR goes, a log line
- Lovelace: not there (yet), a log line says so (the tile still opens the window)

The codes carry the setup passcode: they are drawn and never logged. The text line has the manual pairing code, as the
MATTER panel of the WebUI does

*/

#include "espurna.h"

#if NSPANEL_SUPPORT

#include "espurna_matter.h"
#include "nspanel.h"

#include <qrcode.h>

#include <algorithm>
#include <cstring>

namespace espurna {
namespace nspanel {
namespace qr {
namespace {

constexpr uint32_t CheckMs { 1000 };
constexpr uint32_t RequestMs { 10000 };   // the tile asked for the window: how long the QR waits for it to open

constexpr int Quiet { 4 };              // modules of white around the code
constexpr int TextHeight { 26 };        // the line under it, font 1 (18 px)
constexpr int TextFont { 1 };
constexpr uint16_t White { 65535 };
constexpr uint16_t Black { 0 };

// ECC low: the largest modules (a 22 character MT: payload is version 1). Capacity of versions 1..5 at ECC low
constexpr uint8_t VersionMax { 5 };
constexpr uint16_t AlphanumericLow[VersionMax] { 25, 47, 77, 114, 154 };
constexpr uint16_t BytesLow[VersionMax] { 17, 32, 53, 78, 106 };
constexpr int SizeMax { 17 + 4 * VersionMax };
constexpr size_t BufferSize { ((SizeMax * SizeMax) + 7) / 8 };
constexpr size_t RunsMax { (SizeMax + 1) / 2 };

bool manual { false };          // NSPANEL.QR: shown whatever the window
bool dismissed { false };       // touched away / NSPANEL.QR off while the window stays open
bool window_was { false };
bool shown { false };
int refusal { 0 };              // nspanelScreenShow() said so last, logged once
uint32_t last_check { 0 };
uint32_t requested { 0 };       // "matter-pair" tile: since when the window is asked for, 0 none (it opens in the
                                // Matter task, the QR shows before its open event)

bool alphanumeric(const char* text) {
    for (const char* p = text; *p; ++p) {
        const char c = *p;
        if (!((c >= '0') && (c <= '9')) && !((c >= 'A') && (c <= 'Z')) && !std::strchr(" $%*+-./:", c)) {
            return false;
        }
    }
    return true;
}

// the smallest version that holds the text, 0 when none up to VersionMax does
uint8_t version(const char* text) {
    const auto length = std::strlen(text);
    const auto* capacity = alphanumeric(text) ? AlphanumericLow : BytesLow;
    for (uint8_t index = 0; index < VersionMax; ++index) {
        if (length <= capacity[index]) {
            return index + 1;
        }
    }
    return 0;
}

// "34970112332" -> "3497-011-2332", as the controller apps print it
void manualText(const char* code, char* out, size_t size) {
    if (std::strlen(code) == 11) {
        snprintf_P(out, size, PSTR("%.4s-%.3s-%.4s"), code, code + 4, code + 7);
    } else {
        snprintf_P(out, size, PSTR("%s"), code);
    }
}

void message(int width, int height, const char* text) {
    nspanelScreenFill(0, 0, width, height, White);
    nspanelScreenText(0, 0, width, height, TextFont, Black, White, text);
}

// dark modules as rectangles: a horizontal run is one, the same columns in the next row make it taller
struct Run {
    uint8_t x;
    uint8_t w;
    uint8_t y;
    uint8_t h;
};

size_t drawModules(QRCode& code, int left, int top, int module) {
    Run open[RunsMax];
    size_t open_count { 0 };
    size_t fills { 0 };

    const int size = code.size;
    for (int y = 0; y <= size; ++y) {
        Run next[RunsMax];
        size_t next_count { 0 };
        bool taken[RunsMax] {};

        // the row's runs (none after the last row: everything open is drawn)
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

            if (next_count < RunsMax) {
                next[next_count++] = run;
            }
        }

        // the runs of the row above that end here
        for (size_t index = 0; index < open_count; ++index) {
            if (!taken[index]) {
                const auto& run = open[index];
                nspanelScreenFill(left + run.x * module, top + run.y * module, run.w * module, run.h * module, Black);
                ++fills;
            }
        }

        std::copy(next, next + next_count, open);
        open_count = next_count;
    }

    return fills;
}

// nspanelScreenShow(): on the show and every redraw of the driver
void draw(int width, int height) {
    char payload[32] {};
    char code[24] {};
    if (!matterPairingCodes(payload, sizeof(payload), code, sizeof(code))) {
        message(width, height, "Matter: no pairing codes");
        DEBUG_MSG_P(PSTR("[NSPANEL] QR screen: no Matter pairing codes\n"));
        return;
    }

    const auto ver = version(payload);
    uint8_t buffer[BufferSize];
    QRCode qrcode;
    if (!ver || (qrcode_initText(&qrcode, buffer, ver, ECC_LOW, payload) != 0)) {
        std::memset(payload, 0, sizeof(payload));
        std::memset(code, 0, sizeof(code));
        message(width, height, "Matter: the QR code does not fit");
        DEBUG_MSG_P(PSTR("[NSPANEL] QR screen: the payload does not fit a QR code up to version %u\n"), VersionMax);
        return;
    }
    std::memset(payload, 0, sizeof(payload));

    const int total = qrcode.size + 2 * Quiet;
    const int module = std::min(width / total, (height - TextHeight) / total);
    const int side = total * module;
    const int left = (width - side) / 2 + Quiet * module;
    const int top = (height - TextHeight - side) / 2 + Quiet * module;

    // the white screen is the quiet zone
    nspanelScreenFill(0, 0, width, height, White);
    const auto fills = drawModules(qrcode, left, top, module) + 1;
    std::memset(buffer, 0, sizeof(buffer));

    char text[64];
    char grouped[32];
    manualText(code, grouped, sizeof(grouped));
    snprintf_P(text, sizeof(text), PSTR("Matter: scan to pair, or code %s"), grouped);
    nspanelScreenText(0, height - TextHeight, width, TextHeight, TextFont, Black, White, text);
    std::memset(code, 0, sizeof(code));
    std::memset(grouped, 0, sizeof(grouped));
    std::memset(text, 0, sizeof(text));

    DEBUG_MSG_P(PSTR("[NSPANEL] QR shown, %u fills, module %d px\n"), static_cast<unsigned>(fills), module);
}

// a touch (or another page) ended it
void closed() {
    shown = false;
    dismissed = true;
    manual = false;
    requested = 0;
    DEBUG_MSG_P(PSTR("[NSPANEL] QR screen closed\n"));
}

void hide() {
    if (shown) {
        shown = false;
        nspanelScreenHide();
        DEBUG_MSG_P(PSTR("[NSPANEL] QR screen gone\n"));
    }
}

// 0 shown, else why not (nspanelScreenShow())
int show() {
    const auto result = nspanelScreenShow(draw, closed);
    shown = (result == 0);
    if (result != refusal) {
        refusal = result;
        if (result == 2) {
            DEBUG_MSG_P(PSTR("[NSPANEL] The QR screen is OHpanel only for now, the Lovelace TFT does not show it\n"));
        }
    }
    return result;
}

void loop() {
    const auto now = millis();
    if (now - last_check < CheckMs) {
        return;
    }
    last_check = now;

    const bool window = matterWindowOpen();
    if (window && !window_was) {
        dismissed = false;
    }
    window_was = window;

    // the tile's window: open now, or it did not open in time
    if (requested && (window || (now - requested >= RequestMs))) {
        if (!window) {
            DEBUG_MSG_P(PSTR("[NSPANEL] The Matter pairing window did not open in %u s\n"),
                static_cast<unsigned>(RequestMs / 1000));
        }
        requested = 0;
    }

    const bool wanted = manual || (window && !dismissed) || requested;
    if (!wanted) {
        hide();
        return;
    }

    // not on the display (yet, or any more: the display came back, another driver): again
    if (!shown || !nspanelScreenShown()) {
        show();
    }
}

// the "matter-pair" tile (nspanelAction()): the basic pairing window through the matter module, the QR at the next loop
void pair() {
    if (!matterWindowStart()) {
        DEBUG_MSG_P(PSTR("[NSPANEL] Matter pairing tile: Matter is not started\n"));
        return;
    }

    requested = millis() | 1;
    dismissed = false;
    last_check = millis() - CheckMs;
    DEBUG_MSG_P(PSTR("[NSPANEL] Matter pairing window asked for by the tile\n"));
}

#if TERMINAL_SUPPORT

// nspanel.qr [off]
void terminalQr(espurna::terminal::CommandContext&& ctx) {
    if ((ctx.argv.size() == 2) && (ctx.argv[1] == F("off"))) {
        manual = false;
        dismissed = true;
        requested = 0;
        hide();
        terminalOK(ctx);
        return;
    }

    if (ctx.argv.size() != 1) {
        terminalError(ctx, F("nspanel.qr [off]"));
        return;
    }

    char payload[32] {};
    char code[24] {};
    const bool codes = matterPairingCodes(payload, sizeof(payload), code, sizeof(code));
    std::memset(payload, 0, sizeof(payload));
    std::memset(code, 0, sizeof(code));
    if (!codes) {
        terminalError(ctx, F("Matter is not started"));
        return;
    }

    switch (show()) {
    case 0:
        manual = true;
        dismissed = false;
        terminalOK(ctx);
        break;
    case 2:
        terminalError(ctx, F("the QR screen is OHpanel only for now"));
        break;
    default:
        terminalError(ctx, F("display not ready"));
        break;
    }
}

PROGMEM_STRING(PanelQr, "NSPANEL.QR");

static constexpr espurna::terminal::Command Commands[] PROGMEM {
    {PanelQr, terminalQr},
};

#endif // TERMINAL_SUPPORT

} // namespace
} // namespace qr
} // namespace nspanel
} // namespace espurna

void nspanelQrSetup() {
#if TERMINAL_SUPPORT
    espurna::terminal::add(espurna::nspanel::qr::Commands);
#endif
    nspanelAction("matter-pair", espurna::nspanel::qr::pair);
    espurnaRegisterLoop(espurna::nspanel::qr::loop);
}

#endif // NSPANEL_SUPPORT
