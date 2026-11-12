/*

OTA FETCH MODULE (every chip)

The board takes its update by an URL, http:// or https://, one way on every chip:
- the request: the WebUI (/ota/fetch of ota_web.cpp, the release list of fwinstall.mjs), the `ota <url> [sha256]`
  terminal command, MQTT `<root>/ota` (OTA_MQTT_SUPPORT): the URL and the SHA-256 of the file (may be empty), the same
  checks everywhere (libs/fetch_check.h)
- the download: the one HTTP client (libs/http_client.h), the loop drives it, nothing waits. The file is a head image,
  what /upgrade takes on this chip (the MODULES panel sends the modules of an install image to /modules/apply)
- where it is written is what the chip forces:
  - ESP8266, Beken: Update, in the next boot, which only has Wi-Fi and telnet (the unstable mode: the ~27 KB of heap
    TLS needs stay free, no module starts). One attempt per stored URL: it is erased before the download. The result
    goes to otaFetchResult (/ota/status), then the board restarts into the new image, or back after a failure
  - ESP32: ESPurna runs from its only application slot and cannot write it: the URL goes to safeboot
    (arch/esp32/safeboot_esp32.cpp), which downloads it with the same client into app0 and boots it

*/

#include "espurna.h"

#include "espurna_ota.h"
#include "ota_driver.h"
#include "system.h"

#if MQTT_SUPPORT && OTA_MQTT_SUPPORT
#include "mqtt.h"
#endif

#if TERMINAL_SUPPORT
#include "terminal.h"
#endif

#if SAFEBOOT_SUPPORT
#include "safeboot_esp32.h"
#endif

#include <memory>

#include "libs/PrintString.h"
#include "libs/fetch_check.h"
#include "libs/http_client.h"
#include "libs/text.h"

namespace espurna {
namespace ota {
namespace fetch {
namespace {

namespace keys {

PROGMEM_STRING(Url, "otaFetchUrl");
PROGMEM_STRING(Sha256, "otaFetchSha256");
PROGMEM_STRING(Result, "otaFetchResult");

} // namespace keys

using espurna::fetch_check::UrlMax;

// What the stored settings say, and what this boot took from them
namespace internal {

String result;

} // namespace internal

void storeResult(const String& value) {
    internal::result = value;
    setSetting(FPSTR(keys::Result), value);
    eepromCommit();
}

// also inside <script> of a page (libs/text.h)
using espurna::text::jsonString;

#if !SAFEBOOT_SUPPORT

// -----------------------------------------------------------------------------
// the download into Update (ESP8266, Beken)
// -----------------------------------------------------------------------------

String updateError() {
    PrintString out(64);
    Update.printError(out);
    String result(out.c_str());
    result.trim();
    return result;
}

// Update is started for one byte more than the file: it is never 'finished' by itself, so a file with the wrong
// SHA-256 (the client checks it before end()) is dropped by end(false) and never booted. The first bytes are checked
// as /upgrade checks them (the image header; Beken: the bootloader the file is for)
class UpdateSink : public http::Sink {
public:
    UpdateSink() = default;
    UpdateSink(const UpdateSink&) = delete;
    UpdateSink& operator=(const UpdateSink&) = delete;

    ~UpdateSink() {
        drop();
    }

    void drop() {
        if (_running) {
            _running = false;
            Update.end(false);
        }
    }

    const char* begin(const http::Head& head) override {
        if (head.status != 200) {
            _error = String(F("HTTP ")) + String(head.status, 10);
            return _error.c_str();
        }

        if (head.length <= 0) {
            _error = F("no Content-Length in the response");
            return _error.c_str();
        }

        _length = static_cast<size_t>(head.length);
        _first = true;
        return nullptr;
    }

    const char* write(const uint8_t* data, size_t length) override {
        if (_first) {
            _first = false;
            if (!driver::verify_header(data, length)) {
                _error = F("not a firmware image for this chip");
                return _error.c_str();
            }

            const auto* error = driver::upload_check(data, length);
            if (error) {
                _error = FPSTR(error);
                return _error.c_str();
            }

            if (!Update.begin(_length + 1)) {
                _error = String(F("the file (")) + String(_length, 10) + F(" bytes) does not fit: ") + updateError();
                return _error.c_str();
            }

            _running = true;
        }

        if (Update.write(const_cast<uint8_t*>(data), length) != length) {
            _error = String(F("write failed: ")) + updateError();
            return _error.c_str();
        }

        return nullptr;
    }

    const char* end() override {
        _running = false;
        if (!Update.end(true)) {
            _error = String(F("image check failed: ")) + updateError();
            return _error.c_str();
        }

        return nullptr;
    }

private:
    String _error;
    size_t _length { 0 };
    bool _first { true };
    bool _running { false };
};

http::Client client;
UpdateSink sink;

// -----------------------------------------------------------------------------
// at boot
// -----------------------------------------------------------------------------

constexpr unsigned long WifiWait { 3 * 60 * 1000 };
constexpr unsigned long Settle { 3000 };

struct Pending {
    String url;
    String sha256;
    bool active { false };
    bool forced { false };
    unsigned long start { 0 };
    unsigned long connected { 0 };
};

Pending pending;

void finish(bool ok, const String& message) {
    pending.active = false;

    storeResult(ok ? (String(F("ok: ")) + message) : (String(F("failed: ")) + message));
    DEBUG_MSG_P(PSTR("[OTA] %s %s\n"), ok ? PSTR("Done:") : PSTR("Failed:"), message.c_str());

#if defined(ESP8266)
    if (!ok) {
        eepromRotate(true);
    }
#endif

#if SYSTEM_CHECK_ENABLED
    // back to the normal mode with the next boot
    if (pending.forced) {
        systemForceStable();
    }
#endif

    if (ok) {
        prepareReset(CustomResetReason::Ota);
    } else if (pending.forced) {
        prepareReset(CustomResetReason::Stability);
    }
}

void loop() {
    if (!pending.active) {
        return;
    }

    if (!wifiConnected()) {
        pending.connected = 0;
        if (millis() - pending.start > WifiWait) {
            finish(false, F("no Wi-Fi connection"));
        }
        return;
    }

    if (!pending.connected) {
        pending.connected = millis();
        return;
    }

    if (millis() - pending.connected < Settle) {
        return;
    }

    pending.active = false;

#if defined(ESP8266)
    // Update writes over the second EEPROM rotation sector at the end of the free space (1 MB layout)
    eepromRotate(false);
#endif

    const bool started = client.start(pending.url.c_str(), pending.sha256.c_str(), sink,
        [](const http::Result& result) {
            if (!result.ok) {
                sink.drop();
            }
            finish(result.ok, result.message);
        });
    pending.url = String();
    if (started) {
        espurnaRegisterLoop([]() {
            client.loop();
        });
    }
}

#endif // !SAFEBOOT_SUPPORT

// The request is taken once, a broken URL never keeps the board busy over the next boots
void setup() {
    internal::result = getSetting(FPSTR(keys::Result));

#if !SAFEBOOT_SUPPORT
    const auto url = getSetting(FPSTR(keys::Url));
    if (!url.length()) {
        return;
    }

    pending.url = url;
    pending.sha256 = getSetting(FPSTR(keys::Sha256));
    delSetting(FPSTR(keys::Url));
    delSetting(FPSTR(keys::Sha256));
    eepromCommit();

    pending.active = true;
    pending.start = millis();

#if SYSTEM_CHECK_ENABLED
    // only Wi-Fi and telnet in this boot, the heap stays free for TLS
    systemForceUnstable();
    pending.forced = true;
#endif

    DEBUG_MSG_P(PSTR("[OTA] Update from %s once Wi-Fi is up\n"), url.c_str());
    espurnaRegisterLoop(loop);
#endif
}

// ---- the requests: WebUI, terminal, MQTT ----

#if TERMINAL_SUPPORT
PROGMEM_STRING(OtaCommand, "OTA");

void otaCommand(::terminal::CommandContext&& ctx) {
    if ((ctx.argv.size() != 2) && (ctx.argv.size() != 3)) {
        terminalError(ctx, F("OTA <URL> [<SHA-256>]"));
        return;
    }

    String error;
    if (!otaFetchRequest(ctx.argv[1], (ctx.argv.size() == 3) ? ctx.argv[2] : String(), error)) {
        terminalError(ctx, error);
        return;
    }

    terminalOK(ctx);
    prepareReset(CustomResetReason::Ota);
}

static constexpr ::terminal::Command OtaCommands[] PROGMEM {
    {OtaCommand, otaCommand},
};
#endif

#if MQTT_SUPPORT && OTA_MQTT_SUPPORT
// "<url>" or "<url> <sha256>"
void mqttCallback(unsigned int type, StringView topic, StringView payload) {
    if (type == MQTT_CONNECT_EVENT) {
        mqttSubscribe(MQTT_TOPIC_OTA);
        return;
    }

    if ((type == MQTT_MESSAGE_EVENT) && mqttMagnitude(topic).equals(MQTT_TOPIC_OTA)) {
        const auto text = payload.toString();
        const int space = text.indexOf(' ');
        const auto url = (space > 0) ? text.substring(0, space) : text;
        const auto sha256 = (space > 0) ? text.substring(space + 1) : String();

        String error;
        if (!otaFetchRequest(url, sha256, error)) {
            DEBUG_MSG_P(PSTR("[OTA] MQTT request refused: %s\n"), error.c_str());
            return;
        }

        prepareReset(CustomResetReason::Ota);
    }
}
#endif

} // namespace
} // namespace fetch
} // namespace ota
} // namespace espurna

// the caller restarts the board: the next boot (ESP8266, Beken) or safeboot (ESP32) downloads it
bool otaFetchRequest(const String& url, const String& sha256, String& error) {
    using namespace espurna::ota::fetch;

    if (!espurna::fetch_check::validUrl(url)) {
        error = F("invalid URL (http:// or https://, up to 240 characters)");
        return false;
    }

    if (sha256.length() && !espurna::fetch_check::validSha256(sha256)) {
        error = F("invalid SHA-256 (64 lower case hex digits)");
        return false;
    }

    internal::result = String();

#if SAFEBOOT_SUPPORT
    return safebootUpdateFromUrl(url, sha256, error);
#else
    setSetting(FPSTR(keys::Url), url);
    setSetting(FPSTR(keys::Sha256), sha256);
    delSetting(FPSTR(keys::Result));
    eepromCommit();

    DEBUG_MSG_P(PSTR("[OTA] Stored %s\n"), url.c_str());
    return true;
#endif
}

String otaFetchStatus() {
    using namespace espurna::ota::fetch;

    String out;
    out.reserve(160 + UrlMax);
    out += F("{\"image\":\"" ESPURNA_IMAGE "\",\"fetch\":1,\"free\":");
    out += String(espurna::ota::driver::update_size(), 10);
    out += F(",\"url\":");
    jsonString(out, getSetting(FPSTR(keys::Url)));
    out += F(",\"result\":");
    jsonString(out, internal::result);
    out += '}';

    return out;
}

String otaFetchResult() {
    return espurna::ota::fetch::internal::result;
}

void otaFetchSetup() {
    espurna::ota::fetch::setup();

#if TERMINAL_SUPPORT
    espurna::terminal::add(espurna::ota::fetch::OtaCommands);
#endif

#if MQTT_SUPPORT && OTA_MQTT_SUPPORT
    mqttRegister(espurna::ota::fetch::mqttCallback);
#endif
}
