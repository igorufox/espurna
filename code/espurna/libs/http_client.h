/*

HTTP CLIENT

The one http:// and https:// client of everything ESPurna runs: the head of every chip (ESP8266, ESP32, Beken), the
ESP32 safeboot (code/safeboot-app, plain ESP-IDF) and code/migration-app. One build of it, no variants, no Arduino:
- ESPurna: the head (net.cpp, libs/net_impl.h); the modules call the head's
- code/migration-app, code/safeboot-app: their main.cpp includes the same implementation (libs/http_client_impl.h)

Users: the update by an URL (ota_fetch.cpp: /ota/fetch, the `ota <url>` command, MQTT ota; the safeboot; the migration image),
the NSPanel (weather, the URL requests of its screens, the TFT upload), ThingSpeak and InfluxDB (POST).

- the connection is lwIP's raw TCP API, which every one of them has (libs/tcp.h, under it libs/tls.h). The lwIP
  callbacks only copy what arrived into a ring buffer, the TCP window stays closed until the loop took the bytes (no tcp_recved() before); a segment that does not fit is refused, lwIP gives it again. Nothing else runs
  there. The pcb is touched only in lwIP's context: directly without an OS (ESP8266 NONOS, NO_SYS), else through
  tcpip_api_call()
- the loop drives everything else in small steps (`loop()`): TLS, the request, the response head, the body into the
  sink. A step never waits
- TLS is BearSSL everywhere, its engine has no I/O of its own (bytes in, bytes out): the ESP8266 core has it, the others
  take the same sources as a library (igorufox/bearssl-esp8266). The certificate chain is not validated: a download is
  checked against its SHA-256 instead (from the release list, which the browser took over validated https).
  The base suites: TLS 1.2, static RSA + AES-128-GCM (GitHub Pages offers it, the least code), the server's RSA key
  taken from its certificate
- ESPurna's tls module (module_tls.cpp) adds the rest for any server: ECDHE with RSA and ECDSA certificates,
  ChaCha20-Poly1305, the X.509 decoder of BearSSL for the server key. Installed (Apply + reboot as every module), every
  connection of the head's client offers its suites. The TLS itself is libs/tls.h (shared with MQTT), the hooks
  are espurna::tls::extend() and espurna::tls::key() there
- what differs per chip is only what the chip forces: the entropy for TLS, and on the ESP8266 the second stack for
  the RSA, EC and record crypto steps (its loop stack is 4 KB, RSA i15 alone takes ~4.5 KB)

Request: GET or POST (with its body and Content-Type), Range from a byte. Response: the status, Content-Length or
chunked or until the server closes, redirects (up to 4, absolute or a path; a POST is not repeated), the SHA-256 of the
body when one is given. The sink gets the head first and decides (a download takes 200 only), then the body;
`writable()` of the sink holds the body back while it cannot take more (the TFT upload into a UART).

    struct Sink : espurna::http::Sink { ... };
    espurna::http::Client client;
    client.start(url, sha256 (or nullptr), sink, [](const espurna::http::Result& result) { ... });
    ... client.loop() from the loop ...

*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

// the stream under it: TCP, and TLS for https (BearSSL)
#include "tls.h"

namespace espurna {
namespace http {

// --- small things, also of code/migration-app ---------------------------------------------------------------------------

// decimal, after spaces (atoi of newlib links strtol and errno)
inline long number(const char* text) {
    while (*text == ' ') {
        ++text;
    }

    long out { 0 };
    while ((*text >= '0') && (*text <= '9')) {
        out = (out * 10) + (*text++ - '0');
    }

    return out;
}

// lower case into `out` (2 * len + 1 bytes), as the SHA-256 of the release list
inline void hex(const uint8_t* data, size_t len, char* out) {
    static const char Digits[] = "0123456789abcdef";
    for (size_t index = 0; index < len; ++index) {
        *out++ = Digits[data[index] >> 4];
        *out++ = Digits[data[index] & 15];
    }
    *out = '\0';
}

constexpr size_t UrlMax { 256 };
constexpr size_t MessageMax { 96 };

// --- the response and its body -----------------------------------------------------------------------------------

struct Head {
    int status { 0 };
    long length { -1 };     // -1: not given (chunked, or until the server closes)
    bool chunked { false };
    long range { -1 };      // 206: the first byte of Content-Range ("bytes <first>-..."), -1 otherwise
    char location[UrlMax] {};
};

// Where the body goes. begin() sees the head first: an error (PSTR or a message) stops the request. writable(): bytes
// the sink takes now (0 holds the body back). end() after the last byte
struct Sink {
    virtual ~Sink() = default;
    virtual const char* begin(const Head& head) = 0;
    virtual const char* write(const uint8_t* data, size_t len) = 0;
    virtual size_t writable() {
        return SIZE_MAX;
    }
    virtual const char* end() = 0;
};

// the status only, the body is dropped
struct Discard : public Sink {
    const char* begin(const Head&) override {
        return nullptr;
    }

    const char* write(const uint8_t*, size_t) override {
        return nullptr;
    }

    const char* end() override {
        return nullptr;
    }
};

struct Result {
    bool ok { false };
    int status { 0 };
    size_t length { 0 };
    char message[MessageMax] {};
};

using Done = std::function<void(const Result&)>;

struct Request {
    const char* url { nullptr };
    const char* sha256 { nullptr };     // nullptr / empty, or the lower case hex of the body
    const char* method { "GET" };       // "GET" or "POST"
    const char* type { nullptr };       // Content-Type of the body
    const uint8_t* body { nullptr };    // copied by start()
    size_t body_len { 0 };
    uint32_t from { 0 };                // > 0: only the file from that byte on (Range): 206 (or 200 with all of it)
    const char* headers { nullptr };    // more lines of the head, each "Name: value\r\n" (NoFUSS), up to 255 bytes
};

// --- the client --------------------------------------------------------------------------------------------------

class Client {
public:
    Client();
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // `done` once, from loop() (or from start() when it fails at once: then start() returns false)
    bool start(const Request& request, Sink& sink, Done done);

    // GET `url`, the body into `sink`
    bool start(const char* url, const char* sha256, Sink& sink, Done done, uint32_t from = 0) {
        Request request;
        request.url = url;
        request.sha256 = sha256;
        request.from = from;
        return start(request, sink, std::move(done));
    }

    bool busy() const;
    void abort();

    // from the loop: one step of whatever is due, never waiting
    void loop();

    class Impl;

private:
    std::unique_ptr<Impl> _impl;
};

} // namespace http
} // namespace espurna
