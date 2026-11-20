/*

HTTP CLIENT

The one http:// and https:// client of everything ESPurna runs: the head of every chip (ESP8266, ESP32, Beken) and
code/migration-app. One build of it, no variants, no Arduino:
- ESPurna: the http module (http.cpp, module_http.cpp); the modules that use it need it (head.h) and link against it
- code/migration-app: its main.cpp includes the same implementation (libs/http_client_impl.h)

Users: the migration image (its download of the full image), the NSPanel (weather, the URL requests of its screens, the TFT upload), ThingSpeak and InfluxDB (POST).

- the connection is lwIP's raw TCP API, which every one of them has (libs/tcp.h, under it libs/tls.h). The lwIP
  callbacks only copy what arrived into a ring buffer, the TCP window stays closed until the loop took the bytes (no tcp_recved() before); a segment that does not fit is refused, lwIP gives it again. Nothing else runs
  there. The pcb is touched only in lwIP's context: directly without an OS (ESP8266 NONOS, NO_SYS), else through
  tcpip_api_call()
- the loop drives everything else in small steps (`loop()`): TLS, the request, the response head, the body into the
  sink. A step never waits
- TLS is libs/tls.h (shared with MQTT): mbedtls of the chip's SDK in the tls module (module_tls.cpp); without the
  module an https request fails, http works. The certificate chain is not validated for a download: it is checked
  against its SHA-256 instead (from the release list, which the browser took over validated https)

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
#include <cstring>
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

// `text` into `out` of `size` bytes, cut and always terminated (nullptr: empty). Shared with the HTTP server
inline void copy(char* out, size_t size, const char* text) {
    if (!size) {
        return;
    }

    out[0] = '\0';
    if (text) {
        strncpy(out, text, size - 1);
        out[size - 1] = '\0';
    }
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

// the NSPanel's Open-Meteo request is ~290 bytes (nspanel_net.cpp, url[384]): 256 refused it as "invalid URL" (11-16)
constexpr size_t UrlMax { 384 };
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
    const char* headers { nullptr };    // more lines of the head, each "Name: value\r\n" , up to 255 bytes
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
