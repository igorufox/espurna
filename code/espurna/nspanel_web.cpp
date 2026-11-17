/*

NSPANEL MODULE - config upload and web

POST /nspanel.json hand-over to loop(), the /nspanel editor, /nspanel.json, /nspanel-status, and the
WebUI NSPANEL section with its screen mirror

*/

#include "espurna.h"

#if NSPANEL_SUPPORT

#include "nspanel_core.h"

#if WEB_SUPPORT
#include "nspanel_html.h"
#include "web.h"
#include "ws.h"
#endif

#include <ArduinoJson.h>
#include <StreamString.h>

#include "espurna_tasks.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

namespace espurna {
namespace nspanel {

// -----------------------------------------------------------------------------
// Config upload hand-over (POST /nspanel.json)
// -----------------------------------------------------------------------------

// The body handler only collects the body; validate + save + apply run in loop() (ref. upload::process()) and the
// HTTP answer goes out once the result is known. Everything runs in loop() (the web server of libs/http_server.h)
namespace upload {
namespace {

enum class State : uint8_t {
    Idle,
    Queued,  // body complete, waiting for loop()
    Done,    // code / message ready for the client
};

struct Job {
    State state { State::Idle };
    uint32_t id { 0 };
    uint32_t queued_ms { 0 };
    std::unique_ptr<char[]> buffer;
    size_t size { 0 };
    int code { 0 };
    String message;
};

Job job;

// a body is being received into the async_tcp buffer (read by loop() without the lock)
::espurna::system::Shared<bool> receiving { false };

} // namespace

// no weather TLS session (40-60 KB) is started while a config is on its way
bool busy() {
    return receiving.load() || (job.state == State::Queued);
}


// loop(): validate, save and apply a received config, the answer is picked up by onUploadPoll()
void process() {
    if (job.state != State::Queued) {
        return;
    }

    // its TLS session takes 40-60 KB, the parse goes after it (the crash of 2026-10-25)
    if (weather::running() && ((millis() - job.queued_ms) < build::UploadWeatherWaitMs)) {
        return;
    }

    int code = 500;
    String message;
    String error;
    String warnings;

    ESPURNA_TRY {
        // compiled straight into the flash (no heap copy of the image)
        const auto result = storage::save(job.buffer.get(), job.size, error, warnings);
        job.buffer.reset(); // the parse destroyed it

        if (result == ParseResult::WriteFailed) {
            message = String(F("ERROR: write failed: ")) + error;
        } else if (result != ParseResult::Ok) {
            code = (result == ParseResult::LowMemory) ? 503 : 400;
            message = String(F("ERROR: ")) + error;
        } else {
            code = 200;
            message = F("OK");
            if (warnings.length()) {
                message += F(", warnings: ");
                message += warnings;
            }

            Config next;
            String ignored;
            if (!storage::open(next, error, ignored)) {
                message = String(F("OK, saved; it cannot be read back: ")) + error;
            } else {
                setLoadError(String());
                applyConfig(std::move(next), warnings, build::ConfigPath);
            }
        }
    } ESPURNA_CATCH_BAD_ALLOC {
        // the image is saved before applyConfig() runs
        if (code == 200) {
            message = F("OK, saved; the display reload ran out of memory (try nspanel.reload)");
        } else {
            code = 503;
            message = F("ERROR: out of memory, retry");
        }
    }

    DEBUG_MSG_P(PSTR("[NSPANEL] Config upload: HTTP %d %s\n"), code, message.c_str());

    job.buffer.reset();
    job.size = 0;
    job.code = code;
    job.message = std::move(message);
    job.state = State::Done;
}

} // namespace upload

// -----------------------------------------------------------------------------
// Web
// -----------------------------------------------------------------------------

#if WEB_SUPPORT

namespace {

void sendText(WebRequest& request, int code, const String& message) {
    if (code == 503) {
        request.addHeader("Retry-After", "2");
    }
    webSend(request, code, "text/plain; charset=utf-8", message);
}

void onEditor(WebRequest& request) {
    if (!webAuthenticate(request)) {
        webRequestAuthentication(request);
        return;
    }

    const auto* page = web::EditorPage;
    const size_t size = strlen_P(page);
    request.send(200, "text/html; charset=utf-8", static_cast<long>(size),
        [page, size](uint8_t* out, size_t max, size_t index) -> size_t {
            const size_t take = (index < size) ? std::min(max, size - index) : 0;
            if (take) {
                memcpy_P(out, page + index, take);
            }
            return take;
        });
}

// GET /nspanel.json: written back from the running config's image (the saved one, or the built-in), piece by piece:
// each piece the server asks for runs the writer from the start and keeps the bytes of that piece, nothing of the
// text is held. A config applied meanwhile ends the answer early
struct JsonPiece {
    uint8_t* out;
    size_t max;
    size_t skip;    // bytes before the piece
    size_t taken;
};

void countPiece(void* ctx, const char*, size_t length) {
    *static_cast<size_t*>(ctx) += length;
}

void takePiece(void* ctx, const char* data, size_t length) {
    auto* piece = static_cast<JsonPiece*>(ctx);
    if (piece->skip >= length) {
        piece->skip -= length;
        return;
    }

    data += piece->skip;
    length -= piece->skip;
    piece->skip = 0;

    const size_t take = std::min(length, piece->max - piece->taken);
    std::memcpy(piece->out + piece->taken, data, take);
    piece->taken += take;
}

void onGetConfig(WebRequest& request) {
    if (!webAuthenticate(request)) {
        webRequestAuthentication(request);
        return;
    }

    const auto* image = reinterpret_cast<const uint8_t*>(config().image);
    if (!image) {
        webSend(request, 503, "text/plain; charset=utf-8", F("ERROR: no config loaded"));
        return;
    }

    size_t size = 0;
    writeJson(image, countPiece, &size);

    const auto generation = configGeneration();
    request.send(200, "application/json; charset=utf-8", static_cast<long>(size),
        [generation, size](uint8_t* out, size_t max, size_t index) -> size_t {
            const auto* image = reinterpret_cast<const uint8_t*>(config().image);
            if ((generation != configGeneration()) || !image || (index >= size)) {
                return 0;
            }

            JsonPiece piece { out, std::min(max, size - index), index, 0 };
            writeJson(image, takePiece, &piece);
            return piece.taken;
        });
}

// POST /nspanel.json, ref. namespace upload. The body is collected here, loop() parses it (upload::process()), the
// answer is sent once the job is done (answerLoop()). Contract of firmware_nspanel_json.py and the editor: 200
// "OK[, warnings: ...]" when saved and applied, 400 "ERROR: ..." for an invalid config, 413 too large, 503 busy / low
// memory (retry). The handlers run in the loop (libs/http_server.h)
struct Receive {
    WebRequest* request { nullptr };
    std::unique_ptr<char[]> buffer;
    size_t size { 0 };          // the text kept: the body without the whitespace outside strings
    size_t total { 0 };         // the body
    size_t received { 0 };
    bool in_string { false };
    bool escaped { false };
    int code { 0 };
    String message;
};

Receive rx;
uint32_t last_id { 0 };

// the request waiting for the result of job 'waiting_id'
WebRequest* waiting { nullptr };
uint32_t waiting_id { 0 };
uint32_t waiting_ms { 0 };

void rxReset() {
    rx.request = nullptr;
    rx.buffer.reset();
    rx.size = 0;
    rx.total = 0;
    rx.received = 0;
    rx.in_string = false;
    rx.escaped = false;
    rx.code = 0;
    rx.message = String();
    upload::receiving.store(false);
}

void rxFail(int code, const __FlashStringHelper* message) {
    rx.code = code;
    rx.message = message ? String(message) : String();
    rx.buffer.reset();
    upload::receiving.store(false);
}

// the request is dropped right after this
void uploadForget(WebRequest* request) {
    if (rx.request == request) {
        rxReset();
    }
    if (waiting == request) {
        waiting = nullptr;
    }
}

void onPostBody(WebRequest& request, const char*, size_t index, const uint8_t* data, size_t len, bool) {
    const size_t total = request.contentLength();
    if (index == 0) {
        // one upload at a time, a second one gets 503 from onPostConfig()
        if ((rx.request && (rx.request != &request)) || waiting) {
            return;
        }
        rxReset();
        rx.request = &request;
        auto* self = &request;
        request.onDisconnect([self]() {
            uploadForget(self);
        });

        if (!webAuthenticate(request)) {
            rxFail(401, nullptr);
            return;
        }

        // the limit is on the text without the whitespace outside strings: the editor's own formatted GET (2-space
        // indents, ~18 KB for the NSPANEL1 config) goes back as it came. The body itself up to 4 times that
        if (!total || (total > (4 * build::ConfigMax))) {
            rxFail(413, F("ERROR: config too large (max 16384 bytes without the whitespace)"));
            return;
        }

        if (upload::job.state == upload::State::Queued) {
            rxFail(503, F("ERROR: another upload is being processed, retry"));
            return;
        }

        // the kept text is the only copy (parsed in place later), at most ConfigMax
        const size_t keep = std::min(total, build::ConfigMax);
        if ((systemHeapStats().usable < (keep + 1 + build::ConfigBlockReserve))
            || (systemFreeHeap() < (keep + build::ConfigHeapReserve)))
        {
            rxFail(503, F("ERROR: low memory, retry"));
            return;
        }

        rx.buffer.reset(new (std::nothrow) char[keep + 1]);
        if (!rx.buffer) {
            rxFail(503, F("ERROR: low memory, retry"));
            return;
        }
        rx.total = total;
        upload::receiving.store(true);
    }

    if ((rx.request != &request) || rx.code) {
        return;
    }

    if (!rx.buffer || (index != rx.received) || ((index + len) > rx.total)) {
        rxFail(400, F("ERROR: unexpected body"));
        return;
    }

    // whitespace outside strings is dropped as it comes
    for (size_t at = 0; at < len; ++at) {
        const char c = static_cast<char>(data[at]);
        if (rx.in_string) {
            if (rx.escaped) {
                rx.escaped = false;
            } else if (c == '\\') {
                rx.escaped = true;
            } else if (c == '"') {
                rx.in_string = false;
            }
        } else if ((c == ' ') || (c == '\n') || (c == '\r') || (c == '\t')) {
            continue;
        } else if (c == '"') {
            rx.in_string = true;
        }

        if (rx.size >= build::ConfigMax) {
            rxFail(413, F("ERROR: config too large (max 16384 bytes without the whitespace)"));
            return;
        }
        rx.buffer[rx.size++] = c;
    }
    rx.received += len;
}

void onPostConfig(WebRequest& request) {
    if (rx.request != &request) {
        if (!webAuthenticate(request)) {
            webRequestAuthentication(request);
            return;
        }
        if (rx.request || waiting || (upload::job.state == upload::State::Queued)) {
            sendText(request, 503, F("ERROR: another upload is in progress, retry"));
            return;
        }
        sendText(request, 400, F("ERROR: empty body"));
        return;
    }

    const auto code = rx.code;
    const auto message = rx.message;
    rx.request = nullptr; // the disconnect callback stays, for 'waiting'

    if (code == 401) {
        rxReset();
        webRequestAuthentication(request);
        return;
    }

    if (code) {
        rxReset();
        sendText(request, code, message.length() ? message : String(F("ERROR: busy, retry")));
        return;
    }

    if (!rx.buffer || (request.bodyReceived() != rx.total) || (rx.received != rx.total)) {
        rxReset();
        sendText(request, 400, F("ERROR: incomplete body"));
        return;
    }

    auto& job = upload::job;
    if (job.state == upload::State::Queued) {
        rxReset();
        sendText(request, 503, F("ERROR: another upload is being processed, retry"));
        return;
    }

    // hand over to upload::process(), a stale unanswered result is dropped
    rx.buffer[rx.size] = '\0';
    job.buffer = std::move(rx.buffer);
    job.size = rx.size;
    job.id = ++last_id;
    job.queued_ms = millis();
    job.code = 0;
    job.message = String();
    job.state = upload::State::Queued;
    rxReset();

    // no answer before loop() has saved and applied it, ref. answerLoop()
    waiting = &request;
    waiting_id = job.id;
    waiting_ms = millis();
}

// the result of the job the waiting request is for
void answerLoop() {
    if (!waiting) {
        return;
    }

    int code = 0;
    String message;
    auto& job = upload::job;
    if (job.id != waiting_id) {
        code = 500;
        message = F("ERROR: upload lost");
    } else if (job.state == upload::State::Done) {
        code = job.code;
        message = std::move(job.message);
        job.message = String();
        job.state = upload::State::Idle;
    } else if ((millis() - waiting_ms) >= build::UploadAnswerMs) {
        job.buffer.reset();
        job.size = 0;
        job.state = upload::State::Idle;
        code = 503;
        message = F("ERROR: busy, retry");
    } else {
        return;
    }

    auto* request = waiting;
    waiting = nullptr;
    sendText(*request, code, message);
}

// active display driver, for the editor's preview
void onStatus(WebRequest& request) {
    if (!webAuthenticate(request)) {
        webRequestAuthentication(request);
        return;
    }

    DynamicJsonBuffer buffer(3072);
    JsonObject& root = buffer.createObject();
    root["display"] = driverPrefName(config().display);
    root["driver"] = link::name();
    root["connected"] = link::connected();
    root["probing"] = link::probing();
    root["version"] = driver::version();
    root["model"] = driver::model();
    const auto design = driver::design();
    if (design.length()) {
        root["design"] = design;
    }
    root["flashing"] = flash::busy();
    {
        // the progress or the last result, the WebUI shows it under "Flash the screen"
        StreamString text;
        flash::info(text);
        text.trim();
        root["flash"] = static_cast<const String&>(text);
    }

    // the icon names of the built-in table, offered by the editor's icon field
    JsonArray& names = root.createNestedArray("icons");
    for (size_t index = 0; index < iconCount(); ++index) {
        names.add(iconName(index));
    }

    String out;
    root.printTo(out);
    webSend(request, 200, "application/json", std::move(out));
}

// the WebUI screen mirror (OHpanel only): "nspanelScreen" starts it, touches come back as the TFT's own events
void onWsAction(uint32_t client_id, const char* action, JsonObject& data) {
    auto* active = link::active();
    // "Flash the screen": the TFT by its URL, as nspanel.flash at its default rate; any screen, also the stock one
    if (!strcmp_P(action, PSTR("nspanelFlash"))) {
        String out;
        const char* link = data["url"] | "";
        if (flash::start(String(link), 921600, out)) {
            out = F("started");
        }
        wsPost(client_id, [out](JsonObject& root) {
            root["nspanelFlash"] = out;
        });
        return;
    }

    if (!strcmp_P(action, PSTR("nspanelScreen"))) {
        if (!active || flash::busy() || !active->mirrorStart()) {
            wsPost(client_id, [](JsonObject& root) {
                root["nspanelTft"] = F("#unsupported\n");
            });
        }
        return;
    }

    if (!active || flash::busy()) {
        return;
    }

    if (!strcmp_P(action, PSTR("nspanelTouch"))) {
        const char* page = data["page"] | "";
        const char* component = data["component"] | "";
        const char* value = data["value"] | "";
        if (*page && *component && *value) {
            active->event(page, component, value);
        }
    } else if (!strcmp_P(action, PSTR("nspanelXY"))) {
        if (data.is<int>("x") && data.is<int>("y")) {
            active->touchXY(data["x"].as<int>(), data["y"].as<int>(), (data["press"] | 0) != 0);
        }
    }
}

// shows the NSPANEL section of the WebUI
void onWsVisible(JsonObject& root) {
    wsPayloadModule(root, STRING_VIEW("nspanel"));
}

} // namespace

void setupWeb() {
    wsRegister()
        .onVisible(onWsVisible)
        .onAction(onWsAction);

    auto& server = webServer();
    server.on("/nspanel-status", WebMethod::Get, onStatus);
    server.on("/nspanel", WebMethod::Get, onEditor);
    server.on("/nspanel.json", WebMethod::Get, onGetConfig);
    server.on("/nspanel.json", WebMethod::Post | WebMethod::Put, onPostConfig, onPostBody);
    espurnaRegisterLoop(answerLoop);
}

#endif // WEB_SUPPORT

} // namespace nspanel
} // namespace espurna

#endif // NSPANEL_SUPPORT
