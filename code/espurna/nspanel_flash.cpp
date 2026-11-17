/*

NSPANEL MODULE - Nextion TFT upload

*/

#include "espurna.h"

#if NSPANEL_SUPPORT

#include "nspanel_core.h"

#include "libs/http_client.h"

#include <cstdarg>
#include <memory>
#include <new>

namespace espurna {
namespace nspanel {

// -----------------------------------------------------------------------------
// Nextion TFT upload (upload protocol v1.2 `whmi-wris`, fallback v1.1 `whmi-wri`)
//
// The upload is steps of the loop (every chip alike, no task of its own): the file comes through the head's one
// asynchronous HTTP client (libs/http_client.h) a block at a time, the sink holds the body back while a block waits
// for the display; the UART steps wait for nothing either, each one checks its time. While it runs the loop does not
// touch the display's UART otherwise (no TX queue, no RX parser, no display probing; panelLoop() returns early).
//
//   (921600, 115200, 9600) FF FF FF, DRAKJHSUYDGBNCJHGJKSHBDN FF FF FF  leave protocol reparse mode
//                        recmod=0 FF FF FF (x2), sleep=0 FF FF FF
//                        connect FF FF FF, FF FF connect FF FF FF     <- comok ...,<flash size> FF FF FF
//                        50 ms, throw-away `sendme` (the display swallows the command after
//                        `connect`, ohpanel-protocol.md 1.3), drain RX for 250 ms
//                        whmi-wris <size>,<baud>,res0 FF FF FF        switch to <baud>, <- 05 (up to 30 s)
//   per 4096-byte block  <block>                                      <- 05 (next), or
//                                                                     <- 08 + uint32 LE offset (v1.2 only:
//                                                                        continue from that file offset,
//                                                                        0 = next block)
// Attempts, each after a fresh handshake, with a display power-cycle between them:
//   1. whmi-wris <size>,<requested baud>,res0   2. whmi-wris <size>,115200,res0
//   3. whmi-wri <size>,115200,res0 (v1.1)       4. whmi-wris <size>,115200,1    5. whmi-wri <size>,115200,1
// Every byte received during the handshake and while waiting for the first 05 is logged in hex
// (first 64 bytes of each phase).
// The handshake only needs the Nextion OS, not the TFT, so it also works after a failed upload.
// When the display does not answer, it is power-cycled (GPIO4) and asked again.
// The HTTP side: the size first (a request stopped after its head), then the file from the 05 on; a request that
// ended before the block was complete is made again from that byte (Range, up to HttpRetries times), an 08 offset
// makes it again from the offset. A server that ignores Range (200) is skipped up to the byte.
// -----------------------------------------------------------------------------

namespace flash {
namespace {

constexpr size_t Block { 4096 };
constexpr size_t HeapMin { 30000 };
// a TLS session with the record size cut by MFLN (libs/tls_impl.h asks for 512, then 4096: GitHub takes either) is
// ~15-20 KB; 65000 was the full 16 KB record and its engine, and refused the screen on the NSPanel at 57 KB free (W78,
// the owner 2026-11-17: lower it)
constexpr size_t HeapMinTls { 40000 };
constexpr uint32_t RunBudgetMs { 30 };
constexpr uint32_t ConnectMs { 1000 };
constexpr uint32_t SettleMs { 250 };
constexpr uint32_t ReadyMs { 30000 };
constexpr uint32_t AckMs { 5000 };
constexpr uint32_t OffsetMs { 10000 };
constexpr int HttpRetries { 3 };
constexpr int PowerCycles { 2 };
constexpr uint32_t PowerOffMs { 1000 };
constexpr uint32_t BootMs { 3000 };
constexpr uint32_t FallbackBaud { 115200 };
constexpr size_t MessageMax { 112 };
constexpr size_t CaptureMax { 64 };

constexpr uint32_t Rates[] { build::OhpanelBaud, build::LovelaceBaud, 9600 };
constexpr size_t RatesCount { sizeof(Rates) / sizeof(Rates[0]) };

enum State : int {
    Idle,
    Running,
    Done,
    Failed,
};

enum Phase : int {
    PhaseNone,
    PhaseHttp,
    PhaseConnect,
    PhaseStart,
    PhaseUpload,
};

const char* phaseName(int value) {
    switch (value) {
    case PhaseHttp:
        return "http";
    case PhaseConnect:
        return "connecting";
    case PhaseStart:
        return "starting";
    case PhaseUpload:
        return "uploading";
    }

    return "-";
}

// the steps of the loop
enum class Step {
    Probe,      // the file size: the head of the first request
    PowerOff,   // the display's supply off
    PowerOn,    // on again, its power-up announcement drained
    Baud,       // a handshake rate set, RX drained
    Wake,       // FF FF FF and the reparse exit sent
    Mode,       // recmod=0 (x2), sleep=0 sent, RX drained
    Comok,      // `connect` sent, waiting for comok
    Settle,     // the rest of comok drained
    Settled,    // `sendme` sent, RX drained
    Ready,      // whmi-wri(s) sent, waiting for 05
    Collect,    // a block from HTTP
    Send,       // the block into the UART
    Ack,        // 05 or 08 for the block
    Offset,     // the 4 bytes after 08
    Reopen,     // the pause before the request is made again
    // after a power cycle only
    Next,       // the next attempt
    Finish,     // the end (failed)
};

enum class Result {
    Ok,
    Fatal, // retrying with another variant would not help
    Retry, // the display did not accept or stopped answering, try the next variant
};

struct Variant {
    const char* command;
    uint32_t rate;
    const char* param;
    bool v12; // whmi-wris: 0x08 offset replies
};

State state { Idle };
int phase { PhaseNone };
uint32_t total { 0 };
uint32_t sent { 0 };
uint32_t baud { 0 };

char url[256];
uint32_t requested { 921600 };
uint32_t started_ms { 0 };
char error[MessageMax] { "" };
char comok[MessageMax] { "" };
char variant[48] { "" };
uint32_t flash_size { 0 };
char last_result[MessageMax] { "never" };

// the steps
Step step { Step::Probe };
uint32_t step_at { 0 };
uint32_t step_ms { 0 };
Step power_then { Step::Baud };

Variant variants[5];
size_t variants_count { 0 };
size_t variant_index { 0 };
int cycle { 0 };
size_t rate_index { 0 };

uint8_t connect_buf[160];
size_t connect_len { 0 };

std::unique_ptr<uint8_t[]> block;
uint32_t pos { 0 };         // the file offset of the block
size_t want { 0 };          // its length
size_t fill { 0 };          // what HTTP gave of it
size_t written { 0 };       // what went into the UART
int retries { 0 };
bool first { true };

// `nspanel.flash <url> check`: the file the same way as an upload, into a CRC-32 instead of the display (nothing is sent
// to it, it is not power-cycled): is what the panel takes the file? (the screen stayed black after uploads that said
// "ok", NSPanel 2026-11-17)
bool check_only { false };
uint32_t check_crc { 0 };

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
    for (size_t n = 0; n < len; ++n) {
        crc ^= data[n];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}
uint32_t upload_start { 0 };
uint32_t next_report { 0 };
uint8_t offset_raw[4];
size_t offset_len { 0 };

void message(const char* format, ...) __attribute__((format(printf, 1, 2)));
void message(const char* format, ...) {
    char buf[MessageMax];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    DEBUG_MSG_P(PSTR("[NSPANEL] Flash: %s\n"), buf);
}

void fail(const char* format, ...) __attribute__((format(printf, 1, 2)));
void fail(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(error, sizeof(error), format, args);
    va_end(args);
    message("%s", error);
}

Stream& port() {
    return uart::port();
}

// no draining here: the display answers right after the switch
void setBaud(uint32_t value) {
    uart::rate(value);
    baud = value;
}

void wait(Step next, uint32_t ms) {
    step = next;
    step_at = millis();
    step_ms = ms;
}

bool waited() {
    return millis() - step_at >= step_ms;
}

// raw RX bytes of the current phase, logged in hex
struct Capture {
    bool active { false };
    uint8_t data[CaptureMax];
    size_t len { 0 };
    size_t total { 0 };
};

Capture capture;

void captureStart() {
    capture.active = true;
    capture.len = 0;
    capture.total = 0;
}

void captureDump(const char* label) {
    capture.active = false;
    if (!capture.total) {
        message("%s: no bytes received", label);
        return;
    }

    message("%s: %u bytes received%s", label, capture.total,
        (capture.total > capture.len) ? ", first 64:" : ":");

    constexpr size_t PerLine { 24 };
    for (size_t at = 0; at < capture.len; at += PerLine) {
        char line[PerLine * 3 + 1];
        size_t out = 0;
        for (size_t n = at; (n < capture.len) && (n < at + PerLine); ++n) {
            out += snprintf(line + out, sizeof(line) - out, "%02X ", capture.data[n]);
        }
        line[out ? out - 1 : 0] = '\0';
        message("  %s", line);
    }
}

int rx() {
    const int byte = port().read();
    if ((byte >= 0) && capture.active) {
        if (capture.len < CaptureMax) {
            capture.data[capture.len++] = byte;
        }
        ++capture.total;
    }
    return byte;
}

void drain() {
    while (rx() >= 0) {
    }
}

int find(const uint8_t* data, size_t len, const char* pattern) {
    const size_t plen = strlen(pattern);
    for (size_t at = 0; (at + plen) <= len; ++at) {
        if (0 == memcmp(data + at, pattern, plen)) {
            return at;
        }
    }
    return -1;
}

// ---- HTTP: the file into the block ----

// The body of the current request. `next` is the file offset of its next byte; the sink takes bytes only while a
// block is being collected at exactly that offset, otherwise it holds the body back
struct Source : public ::espurna::http::Sink {
    const char* begin(const ::espurna::http::Head& head) override {
        if (probe) {
            if ((head.status != 200) || (head.length <= 0)) {
                snprintf(text, sizeof(text), "HTTP %d%s", head.status,
                    (head.status == 200) ? " without Content-Length" : "");
                return text;
            }
            size = head.length;
            return PSTR("size taken");
        }

        if ((head.status == 206) && at) {
            if ((head.range != static_cast<long>(at)) || (head.length != static_cast<long>(size - at))) {
                snprintf(text, sizeof(text), "HTTP 206 with an unexpected range (from %ld, %ld bytes)",
                    head.range, head.length);
                return text;
            }
            next = at;
            skip = 0;
            return nullptr;
        }

        if (head.status == 200) {
            if (head.length != static_cast<long>(size)) {
                snprintf(text, sizeof(text), "file size changed (%ld != %u)", head.length, size);
                return text;
            }
            if (at) {
                message("server ignores Range, skipping %u bytes", at);
            }
            next = 0;
            skip = at;
            return nullptr;
        }

        snprintf(text, sizeof(text), "HTTP %d", head.status);
        return text;
    }

    size_t writable() override {
        if (skip) {
            return skip;
        }
        if ((step != Step::Collect) || (next != pos + fill)) {
            return 0;
        }
        return want - fill;
    }

    const char* write(const uint8_t* data, size_t len) override {
        const size_t dropped = std::min<size_t>(skip, len);
        skip -= dropped;
        next += dropped;
        data += dropped;
        len -= dropped;
        if (!len) {
            return nullptr;
        }

        if ((step != Step::Collect) || (next != pos + fill) || (len > want - fill)) {
            return PSTR("data out of place");
        }

        memcpy(block.get() + fill, data, len);
        fill += len;
        next += len;
        return nullptr;
    }

    const char* end() override {
        return nullptr;
    }

    bool probe { false };
    uint32_t size { 0 };
    uint32_t at { 0 };
    uint32_t next { 0 };
    uint32_t skip { 0 };
    char text[MessageMax];
};

::espurna::http::Client client;
Source source;
String last_http;

void httpDone(const ::espurna::http::Result& result) {
    last_http = result.message;
}

// the file from `at` on; false - not even started
bool open(uint32_t at) {
    client.abort();
    source.probe = false;
    source.at = at;
    source.next = at;
    source.skip = 0;
    return client.start(url, nullptr, source, httpDone, at);
}

// ---- the attempts ----

void finish(bool ok) {
    client.abort();
    block.reset();
    // the check never changed the speed of the port
    if (!check_only) {
        setBaud(build::LovelaceBaud);
    }
    phase = PhaseNone;
    state = ok ? Done : Failed;
}

void powerCycle(Step then) {
    message("power-cycling the display");
    build::powerWrite(HIGH);
    power_then = then;
    wait(Step::PowerOff, PowerOffMs);
}

void connectStart() {
    setBaud(Rates[rate_index]);
    drain();
    wait(Step::Baud, 20);
}

void handshakeStart() {
    phase = PhaseConnect;
    cycle = 0;
    rate_index = 0;
    connectStart();
}

void attemptStart() {
    const auto& how = variants[variant_index];
    message("attempt %u/%u: %s, %s at %u baud", variant_index + 1, variants_count, url, how.command, how.rate);
    handshakeStart();
}

// the attempt is over: the next variant, or the end
void attemptEnd(Result result) {
    if (result == Result::Ok) {
        message("all %u bytes sent in %u s, the display installs the TFT and restarts", total,
            static_cast<unsigned>((millis() - upload_start) / 1000));
        finish(true);
        return;
    }

    client.abort();
    // the check never talks to the display
    if (check_only) {
        finish(false);
        return;
    }

    const bool started = (phase >= PhaseStart);
    const bool last = (result == Result::Fatal) || (++variant_index >= variants_count);

    // after whmi-wri(s) the display waits for data until it is power-cycled
    if (started) {
        phase = PhaseConnect;
        powerCycle(last ? Step::Finish : Step::Next);
        return;
    }

    if (last) {
        finish(false);
        return;
    }

    attemptStart();
}

// the next handshake rate, or a power cycle, or the attempt failed
void connectNext() {
    if (++rate_index < RatesCount) {
        connectStart();
        return;
    }

    if (++cycle <= PowerCycles) {
        rate_index = 0;
        powerCycle(Step::Baud);
        return;
    }

    fail("no 'comok' from the display (921600 / 115200 / 9600 baud, %d power cycles)", PowerCycles);
    attemptEnd(Result::Fatal);
}

// the display answered: the upload command, its 05, the file from byte 0 meanwhile
void uploadStart() {
    if (flash_size && (total > flash_size)) {
        fail("TFT (%u bytes) is larger than the display flash (%u bytes)", total, flash_size);
        attemptEnd(Result::Fatal);
        return;
    }

    const auto& how = variants[variant_index];
    phase = PhaseStart;
    char command[48];
    snprintf(command, sizeof(command), "%s %u,%u,%s", how.command, total, how.rate, how.param);
    snprintf(variant, sizeof(variant), "%s", command);

    // nothing stale may be taken for the acknowledge
    while (port().read() >= 0) {
    }

    message("%s", command);
    uart::writeRaw(command);
    setBaud(how.rate);

    pos = 0;
    fill = 0;
    want = std::min<size_t>(Block, total);
    retries = 0;
    first = true;
    sent = 0;
    if (!open(0)) {
        fail("HTTP request not started");
        attemptEnd(Result::Fatal);
        return;
    }

    // anything before the 05 is noise from the speed change (logged)
    captureStart();
    wait(Step::Ready, ReadyMs);
}

void blockNext() {
    fill = 0;
    written = 0;
    want = std::min<size_t>(Block, total - pos);
    step = Step::Collect;
}

void stepProbe() {
    if (client.busy()) {
        return;
    }

    if (!source.size) {
        fail("%s", last_http.c_str());
        attemptEnd(Result::Fatal);
        return;
    }

    total = source.size;
    if ((total < Block) || (total > (64ul << 20))) {
        fail("implausible TFT size %u bytes", total);
        attemptEnd(Result::Fatal);
        return;
    }

    if (check_only) {
        message("check: %u bytes, no display", total);
        phase = PhaseUpload;
        upload_start = millis();
        next_report = 0;
        pos = 0;
        sent = 0;
        retries = 0;
        first = false;
        check_crc = 0xFFFFFFFFu;
        if (!open(0)) {
            fail("HTTP request not started");
            attemptEnd(Result::Fatal);
            return;
        }
        blockNext();
        return;
    }

    attemptStart();
}

void stepComok() {
    int byte;
    while ((byte = rx()) >= 0) {
        if (connect_len == sizeof(connect_buf)) {
            memmove(connect_buf, connect_buf + sizeof(connect_buf) / 2, sizeof(connect_buf) / 2);
            connect_len = sizeof(connect_buf) / 2;
        }
        connect_buf[connect_len++] = byte;
    }

    const int at = find(connect_buf, connect_len, "comok");
    if (at >= 0) {
        const int end = find(connect_buf + at, connect_len - at, "\xFF\xFF\xFF");
        if (end >= 0) {
            size_t out = 0;
            for (int n = at; (n < at + end) && (out + 1 < sizeof(comok)); ++n) {
                if ((connect_buf[n] >= 0x20) && (connect_buf[n] < 0x7F)) {
                    comok[out++] = connect_buf[n];
                }
            }
            comok[out] = '\0';

            const char* last = strrchr(comok, ',');
            flash_size = last ? strtoul(last + 1, nullptr, 10) : 0;
            message("display answered at %u baud: %s", Rates[rate_index], comok);

            // the rest of the line and the second comok; then a throw-away command,
            // because the display may swallow the command right after `connect`
            wait(Step::Settle, 50);
            return;
        }
    }

    if (waited()) {
        char label[48];
        snprintf(label, sizeof(label), "no comok at %u baud", Rates[rate_index]);
        captureDump(label);
        connectNext();
    }
}

void stepReady() {
    int byte;
    while ((byte = rx()) >= 0) {
        if (byte != 0x05) {
            continue;
        }

        char label[64];
        snprintf(label, sizeof(label), "RX before the first 05 (%u ms)", static_cast<unsigned>(millis() - step_at));
        captureDump(label);

        phase = PhaseUpload;
        upload_start = millis();
        next_report = 0;
        blockNext();
        return;
    }

    if (waited()) {
        captureDump("RX before the first 05 (none)");
        fail("display did not acknowledge '%s' within %u s", variant, ReadyMs / 1000);
        attemptEnd(Result::Retry);
    }
}

void stepCollect() {
    if (fill == want) {
        written = 0;
        step = Step::Send;
        return;
    }

    // the file comes from the right place: the sink takes it
    if (client.busy() && (source.skip || (source.next == pos + fill))) {
        return;
    }

    // an 08 jump: the request again from the offset, nothing lost
    if (client.busy()) {
        if (!open(pos + fill)) {
            fail("HTTP request not started");
            attemptEnd(Result::Fatal);
        }
        return;
    }

    // the request ended before the block was complete (stall, closed, timeout)
    if (++retries > HttpRetries) {
        fail("HTTP read failed at %u: %s", pos + fill, last_http.c_str());
        attemptEnd(Result::Fatal);
        return;
    }

    message("HTTP stalled at %u (%s), reconnecting (%d/%d)", pos + fill, last_http.c_str(), retries, HttpRetries);
    wait(Step::Reopen, 1000 * retries);
}

void stepSend() {
    if (check_only) {
        check_crc = crc32Update(check_crc, block.get(), want);
        pos += want;
        sent = pos;
        const uint32_t percent = (static_cast<uint64_t>(pos) * 100) / total;
        if (percent >= next_report) {
            message("check %u%% (%u / %u bytes, %u s)", percent, pos, total,
                static_cast<unsigned>((millis() - upload_start) / 1000));
            next_report = percent - (percent % 5) + 5;
        }
        if (pos >= total) {
            snprintf(variant, sizeof(variant), "check, CRC32 %08X", static_cast<unsigned>(~check_crc));
            message("check done: %u bytes, CRC32 %08X", total, static_cast<unsigned>(~check_crc));
            finish(true);
            return;
        }
        blockNext();
        return;
    }

    auto& serial = port();
    while (written < want) {
        const int room = serial.availableForWrite();
        const size_t len = std::min<size_t>(want - written, (room > 0) ? room : 256);
        serial.write(block.get() + written, len);
        written += len;
        if (room <= 0) {
            return;
        }
    }

    pos += want;
    sent = pos;

    const uint32_t percent = (static_cast<uint64_t>(pos) * 100) / total;
    if (percent >= next_report) {
        message("%u%% (%u / %u bytes, %u baud, %u s)", percent, pos, total, baud,
            static_cast<unsigned>((millis() - upload_start) / 1000));
        next_report = percent - (percent % 5) + 5;
    }

    wait(Step::Ack, first ? OffsetMs : AckMs);
    first = false;
}

void stepAck() {
    const int ack = rx();
    if (ack < 0) {
        if (waited()) {
            fail("no acknowledge for the block ending at %u", pos);
            attemptEnd(Result::Retry);
        }
        return;
    }

    if (ack == 0x05) {
        if (pos >= total) {
            attemptEnd(Result::Ok);
            return;
        }
        blockNext();
        return;
    }

    if ((ack == 0x08) && variants[variant_index].v12) {
        offset_len = 0;
        wait(Step::Offset, OffsetMs);
        return;
    }

    fail("unexpected reply 0x%02X for the block ending at %u", ack, pos);
    attemptEnd(Result::Retry);
}

void stepOffset() {
    int byte;
    while ((offset_len < sizeof(offset_raw)) && ((byte = rx()) >= 0)) {
        offset_raw[offset_len++] = byte;
    }

    if (offset_len < sizeof(offset_raw)) {
        if (waited()) {
            fail("truncated 0x08 reply at %u", pos);
            attemptEnd(Result::Retry);
        }
        return;
    }

    const uint32_t offset = offset_raw[0]
        | (static_cast<uint32_t>(offset_raw[1]) << 8)
        | (static_cast<uint32_t>(offset_raw[2]) << 16)
        | (static_cast<uint32_t>(offset_raw[3]) << 24);
    if (offset && (offset != pos)) {
        if (offset > total) {
            fail("display asked for offset %u beyond the file (%u)", offset, total);
            attemptEnd(Result::Retry);
            return;
        }

        message("display continues from offset %u (skips %d bytes)", offset,
            static_cast<int>(offset) - static_cast<int>(pos));
        pos = offset;
        sent = pos;
    }

    if (pos >= total) {
        attemptEnd(Result::Ok);
        return;
    }

    blockNext();
}

void run() {
    client.loop();

    switch (step) {
    case Step::Probe:
        stepProbe();
        break;

    case Step::PowerOff:
        if (waited()) {
            build::powerWrite(LOW);
            wait(Step::PowerOn, BootMs);
        }
        break;

    case Step::PowerOn:
        // the power-up announcement, not for us
        drain();
        if (waited()) {
            if (power_then == Step::Finish) {
                finish(false);
            } else if (power_then == Step::Next) {
                attemptStart();
            } else {
                connectStart();
            }
        }
        break;

    case Step::Baud:
        drain();
        if (waited()) {
            static constexpr uint8_t Ff[3] { 0xFF, 0xFF, 0xFF };
            captureStart();
            port().write(Ff, sizeof(Ff));
            uart::writeRaw("DRAKJHSUYDGBNCJHGJKSHBDN");
            wait(Step::Wake, 50);
        }
        break;

    case Step::Wake:
        if (waited()) {
            uart::writeRaw("recmod=0");
            uart::writeRaw("recmod=0");
            uart::writeRaw("sleep=0");
            wait(Step::Mode, 100);
        }
        break;

    case Step::Mode:
        drain();
        if (waited()) {
            static constexpr uint8_t Ff[2] { 0xFF, 0xFF };
            uart::writeRaw("connect");
            port().write(Ff, sizeof(Ff));
            uart::writeRaw("connect");
            connect_len = 0;
            wait(Step::Comok, ConnectMs);
        }
        break;

    case Step::Comok:
        stepComok();
        break;

    case Step::Settle:
        drain();
        if (waited()) {
            uart::writeRaw("sendme");
            wait(Step::Settled, SettleMs);
        }
        break;

    case Step::Settled:
        drain();
        if (waited()) {
            char label[48];
            snprintf(label, sizeof(label), "handshake RX at %u baud", Rates[rate_index]);
            captureDump(label);
            uploadStart();
        }
        break;

    case Step::Ready:
        stepReady();
        break;

    case Step::Collect:
        stepCollect();
        break;

    case Step::Send:
        stepSend();
        break;

    case Step::Ack:
        stepAck();
        break;

    case Step::Offset:
        stepOffset();
        break;

    case Step::Reopen:
        if (waited()) {
            step = Step::Collect;
            if (!open(pos + fill)) {
                fail("HTTP request not started");
                attemptEnd(Result::Fatal);
            }
        }
        break;

    case Step::Next:
    case Step::Finish:
        break;
    }
}

// The drivers take the UART back and the display is detected again: a new TFT may speak the
// other protocol. No raw recmod=1 here (it would break OHpanel's command mode); the Lovelace
// probe sends it at 115200 only, where an OHpanel TFT (921600) cannot parse it
void resume(bool done) {
    while (uart::port().read() >= 0) {
    }
    link::afterFlash(done);
}

} // namespace

// ---- loop side ----

bool busy() {
    return state != Idle;
}

bool check(const String& link, String& out) {
    if (!start(link, 921600, out)) {
        return false;
    }
    check_only = true;
    return true;
}

bool start(const String& link, uint32_t rate, String& out) {
    if (busy()) {
        out = F("an upload is already running");
        return false;
    }

    if (!link.startsWith(F("http://")) && !link.startsWith(F("https://"))) {
        out = F("URL must start with http:// or https://");
        return false;
    }

    if (link.length() >= sizeof(url)) {
        out = F("URL too long");
        return false;
    }

    if (!wifiConnected()) {
        out = F("no WiFi");
        return false;
    }

    const bool https = link.startsWith(F("https://"));
    const auto heap = systemFreeHeap();
    if (heap < (https ? HeapMinTls : HeapMin)) {
        out = String(F("low heap (")) + heap + F(" bytes)");
        if (https) {
            out += F(", try a plain http:// URL");
        }
        return false;
    }

    block.reset(new (std::nothrow) uint8_t[Block]);
    if (!block) {
        out = F("out of memory");
        return false;
    }

    strncpy(url, link.c_str(), sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';
    requested = rate;
    started_ms = millis();
    error[0] = '\0';
    comok[0] = '\0';
    variant[0] = '\0';
    flash_size = 0;
    total = 0;
    sent = 0;
    baud = 0;

    variants_count = 0;
    variant_index = 0;
    // v1.2 `whmi-wris` first. v1.1 `whmi-wri` first (tried 2026-11-17) took all 15.8 MB with every block
    // acknowledged and left the screen dead each time; every working upload of this display went through whmi-wris
    variants[variants_count++] = Variant{"whmi-wris", requested, "res0", true};
    if (requested != FallbackBaud) {
        variants[variants_count++] = Variant{"whmi-wris", FallbackBaud, "res0", true};
    }
    variants[variants_count++] = Variant{"whmi-wri", FallbackBaud, "res0", false};
    variants[variants_count++] = Variant{"whmi-wris", FallbackBaud, "1", true};
    variants[variants_count++] = Variant{"whmi-wri", FallbackBaud, "1", false};

    // the size first: the request stops after its head
    source.probe = true;
    source.size = 0;
    source.at = 0;
    source.next = 0;
    source.skip = 0;
    last_http = String();
    if (!client.start(url, nullptr, source, httpDone)) {
        block.reset();
        out = F("HTTP request not started");
        return false;
    }

    phase = PhaseHttp;
    step = Step::Probe;
    state = Running;
    DEBUG_MSG_P(PSTR("[NSPANEL] Flash: %s, %u baud\n"), url, rate);
    return true;
}

void loop() {
    if (state == Running) {
        // a block takes several steps (collect, send / CRC, the next request); one step per loop pass and the loop's
        // 10 ms pause made the download ~77 KB/s, slower than the display takes at 921600 baud. Steps run back to back
        // while they move on, up to RunBudgetMs; a step that waits (no data yet, no acknowledge) ends the pass
        const auto start = millis();
        for (;;) {
            const auto before_step = step;
            const auto before_pos = pos;
            const auto before_fill = fill;
            const auto before_written = written;
            run();
            if (state != Running) {
                break;
            }
            if ((step == before_step) && (pos == before_pos) && (fill == before_fill) && (written == before_written)) {
                break;
            }
            if ((millis() - start) >= RunBudgetMs) {
                break;
            }
        }
        if (state == Running) {
            return;
        }
    }

    const unsigned seconds = (millis() - started_ms) / 1000;
    const bool ok = (state == Done);
    if (ok) {
        snprintf(last_result, sizeof(last_result), "ok, %u bytes in %u s (%s)", total, seconds, variant);
        DEBUG_MSG_P(PSTR("[NSPANEL] Flash: done (%s)%s\n"), last_result,
            check_only ? "" : ", detecting the display again");
    } else {
        snprintf(last_result, sizeof(last_result), "FAILED after %u s: %s", seconds, error);
        DEBUG_MSG_P(PSTR("[NSPANEL] Flash: %s\n"), last_result);
    }

    state = Idle;
    if (check_only) {
        // the display was never touched: the drivers go on as they were
        check_only = false;
        while (uart::port().read() >= 0) {
        }
        return;
    }
    resume(ok);
}

void info(Print& out) {
    if (state == Running) {
        out.printf_P(PSTR("Flash: %s, %u%% (%u / %u bytes), %u baud, %u s\n"),
            phaseName(phase),
            total ? static_cast<unsigned>((static_cast<uint64_t>(sent) * 100) / total) : 0,
            sent, total, baud,
            static_cast<unsigned>((millis() - started_ms) / 1000));
    } else {
        out.printf_P(PSTR("Flash: idle, last: %s\n"), last_result);
        if (comok[0]) {
            out.printf_P(PSTR("Flash: display %s\n"), comok);
        }
        if (variant[0]) {
            out.printf_P(PSTR("Flash: last command %s\n"), variant);
        }
    }
}

} // namespace flash

} // namespace nspanel
} // namespace espurna

#endif // NSPANEL_SUPPORT
