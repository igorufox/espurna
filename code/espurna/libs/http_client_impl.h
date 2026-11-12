/*

HTTP CLIENT - the implementation (libs/http_client.h), included by exactly one .cpp of an image: ESPurna's
net.cpp (the head, through libs/net_impl.h), code/migration-app's and code/safeboot-app's main.cpp. No Arduino here: lwIP, BearSSL and the C
library, the same on every chip. What that .cpp defines before the include is only how the image logs and locks,
never what the client does (libs/net_impl.h lists them). The connection is libs/tcp.h

*/

#pragma once

#include "http_client.h"
#include "tcp.h"
#include "tls.h"
#include "url_parse.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <new>

#if defined(ESP8266)
#include <pgmspace.h>
#endif

// the flash string calls of the ESP8266; plain ones elsewhere (ESP32 and Beken Arduino define them already)
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef PSTR
#define PSTR(s) (s)
#endif
#ifndef strncpy_P
#define strncpy_P strncpy
#endif
#ifndef strncmp_P
#define strncmp_P strncmp
#endif
#ifndef strncasecmp_P
#define strncasecmp_P strncasecmp
#endif
#ifndef strstr_P
#define strstr_P strstr
#endif
#ifndef vsnprintf_P
#define vsnprintf_P vsnprintf
#endif

namespace espurna {
namespace http {
namespace {

constexpr uint32_t Timeout { 20000 };

using tcp::now;

// --- small things ------------------------------------------------------------------------------------------------

long hexNumber(const char* text) {
    while (*text == ' ') {
        ++text;
    }

    long out { 0 };
    for (;;) {
        const char c = *text++;
        const int digit =
            ((c >= '0') && (c <= '9')) ? (c - '0') :
            ((c >= 'a') && (c <= 'f')) ? (c - 'a' + 10) :
            ((c >= 'A') && (c <= 'F')) ? (c - 'A' + 10) : -1;
        if (digit < 0) {
            return out;
        }
        out = (out * 16) + digit;
    }
}

void copy(char* out, size_t size, const char* text) {
    if (!size) {
        return;
    }

    out[0] = '\0';
    if (text) {
        strncpy(out, text, size - 1);
        out[size - 1] = '\0';
    }
}

struct Target {
    bool https { false };
    char host[url::HostMax] {};
    uint16_t port { 80 };
    char path[url::PathMax] {};
};

bool startsWith(const char* text, const char* prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

// http:// or https:// by the one URL parser (libs/url_parse.h)
bool parse(const char* text, Target& out) {
    url::Parts parts;
    if (!url::parse(text, parts) || !(parts.is("http") || parts.is("https"))) {
        return false;
    }

    out.https = parts.is("https");
    out.port = parts.port;
    copy(out.host, sizeof(out.host), parts.host);
    copy(out.path, sizeof(out.path), parts.path);
    return true;
}

// a redirect target: absolute, or a path on the same server. false - neither (or too long)
bool redirect(const Target& from, const char* location, char* out, size_t size) {
    if (startsWith(location, "https://") || startsWith(location, "http://")) {
        copy(out, size, location);
        return strlen(location) < size;
    }

    if (location[0] == '/') {
        const int len = (from.port != (from.https ? 443 : 80))
            ? snprintf(out, size, "%s%s:%u%s", from.https ? "https://" : "http://", from.host,
                static_cast<unsigned>(from.port), location)
            : snprintf(out, size, "%s%s%s", from.https ? "https://" : "http://", from.host, location);
        return (len > 0) && (static_cast<size_t>(len) < size);
    }

    return false;
}

} // namespace

// --- the client --------------------------------------------------------------------------------------------------

class Client::Impl {
public:
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        close();
    }

    bool start(const Request& request, Sink& sink, Done done) {
        if (busy()) {
            return false;
        }

        _sink = &sink;
        _done = std::move(done);
        _redirects = 0;
        _result = Result{};

        if (!request.url || (strlen(request.url) >= sizeof(_url))) {
            finish(false, PSTR("invalid URL"));
            return false;
        }

        copy(_url, sizeof(_url), request.url);
        copy(_sha256, sizeof(_sha256), request.sha256);
        copy(_method, sizeof(_method), request.method ? request.method : "GET");
        copy(_type, sizeof(_type), request.type);
        copy(_headers, sizeof(_headers), request.headers);
        _from = request.from;

        _body.reset();
        _body_len = 0;
        if (request.body && request.body_len) {
            _body.reset(new (std::nothrow) uint8_t[request.body_len]);
            if (!_body) {
                finish(false, PSTR("out of memory"));
                return false;
            }
            memcpy(_body.get(), request.body, request.body_len);
            _body_len = request.body_len;
        }

        return connect();
    }

    bool busy() const {
        return _state != State::Idle;
    }

    void abort() {
        if (busy()) {
            finish(false, PSTR("aborted"));
        }
    }

    void loop() {
        if (!busy()) {
            return;
        }

        if (now() - _last > Timeout) {
            finish(false, PSTR("timeout"));
            return;
        }

        step();
    }

private:
    enum class State {
        Idle,
        Handshake,
        Request,
        Head,
        Body,
    };

    // --- the stream (libs/tls.h: TCP, and TLS for https) ---------------------------------------------------------

    bool connect() {
        Target target;
        if (!parse(_url, target)) {
            finish(false, PSTR("invalid URL"));
            return false;
        }

        _target = target;
        _head_len = 0;
        _chunk_left = 0;
        _chunk_state = ChunkState::Size;
        _body_done = 0;
        _head = Head{};
        br_sha256_init(&_sha);

        NET_LOG("%s %s", _method, _url);

        _state = State::Handshake;
        _last = now();

        tls::Options options;
        options.secure = _target.https;
        if (!_stream.connect(_target.host, _target.port, options)) {
            streamFailed();
            return false;
        }

        return true;
    }

    // why the stream never got ready
    void streamFailed() {
        switch (_stream.failure()) {
        case tls::Failure::Memory:
            finish(false, PSTR("out of memory"));
            break;
        case tls::Failure::Resolve:
            finishf(false, PSTR("cannot resolve %s"), _target.host);
            break;
        case tls::Failure::None:
        case tls::Failure::Connect:
            finishf(false, PSTR("cannot connect to %s"), _target.host);
            break;
        case tls::Failure::Tls:
        case tls::Failure::Fingerprint:
            finishf(false, PSTR("TLS handshake failed, BearSSL error %d"), _stream.tlsError());
            break;
        }
    }

    // application data out: as much as goes now
    size_t appWrite(const uint8_t* data, size_t len) {
        const auto out = _stream.write(data, len);
        if (out) {
            _last = now();
        }
        return out;
    }

    // application data in: what is there now
    size_t appRead(uint8_t* out, size_t len) {
        const auto got = _stream.read(out, len);
        if (got) {
            _last = now();
        }
        return got;
    }

    // nothing more will come: the server closed and everything it sent was taken
    bool drained() {
        return _stream.drained();
    }

    bool closed() {
        return _stream.drained();
    }

    // --- the steps ----------------------------------------------------------------------------------------------

    void step() {
        switch (_state) {
        case State::Handshake:
            stepHandshake();
            break;
        case State::Request:
            stepRequest();
            break;
        case State::Head:
            stepHead();
            break;
        case State::Body:
            stepBody();
            break;
        case State::Idle:
            break;
        }
    }

    bool post() const {
        return strcmp(_method, "POST") == 0;
    }

    // the request head and the body, in one block
    bool buildRequest() {
        char head[UrlMax + 512];
        int len = snprintf(head, sizeof(head), "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nConnection: close\r\n",
            _method, _target.path, _target.host, HTTP_CLIENT_AGENT);
        if (_from && (len > 0) && (static_cast<size_t>(len) < sizeof(head))) {
            len += snprintf(head + len, sizeof(head) - len, "Range: bytes=%u-\r\n", static_cast<unsigned>(_from));
        }
        if (_headers[0] && (len > 0) && (static_cast<size_t>(len) < sizeof(head))) {
            len += snprintf(head + len, sizeof(head) - len, "%s", _headers);
        }
        if (post() && (len > 0) && (static_cast<size_t>(len) < sizeof(head))) {
            len += snprintf(head + len, sizeof(head) - len, "Content-Type: %s\r\nContent-Length: %u\r\n",
                _type[0] ? _type : "application/x-www-form-urlencoded", static_cast<unsigned>(_body_len));
        }
        if ((len > 0) && (static_cast<size_t>(len) < sizeof(head))) {
            len += snprintf(head + len, sizeof(head) - len, "\r\n");
        }
        if ((len <= 0) || (static_cast<size_t>(len) >= sizeof(head))) {
            finish(false, PSTR("request too long"));
            return false;
        }

        const size_t body = post() ? _body_len : 0;
        _request.reset(new (std::nothrow) uint8_t[len + body]);
        if (!_request) {
            finish(false, PSTR("out of memory"));
            return false;
        }

        memcpy(_request.get(), head, len);
        if (body) {
            memcpy(_request.get() + len, _body.get(), body);
        }
        _request_len = len + body;
        _request_sent = 0;
        return true;
    }

    void stepHandshake() {
        if (!_stream.ready()) {
            if (!_stream.pending()) {
                streamFailed();
            }
            return;
        }

        if (!buildRequest()) {
            return;
        }

        _state = State::Request;
        _last = now();
    }

    void stepRequest() {
        while (_request_sent < _request_len) {
            const auto sent = appWrite(_request.get() + _request_sent, _request_len - _request_sent);
            if (!sent) {
                if (closed()) {
                    finish(false, PSTR("cannot send the request"));
                }
                return;
            }
            _request_sent += sent;
        }

        _request.reset();
        _state = State::Head;
        _last = now();
    }

    // the head line by line, the rest of what came is the beginning of the body
    void stepHead() {
        for (;;) {
            uint8_t c;
            if (!appRead(&c, 1)) {
                if (drained()) {
                    finish(false, PSTR("no HTTP response"));
                }
                return;
            }

            if (c != '\n') {
                if (_head_len + 1 < sizeof(_line)) {
                    _line[_head_len++] = static_cast<char>(c);
                } else {
                    _line_cut = true;
                }
                continue;
            }

            if (_head_len && (_line[_head_len - 1] == '\r')) {
                --_head_len;
            }
            _line[_head_len] = '\0';
            const bool whole = !_line_cut;
            _head_len = 0;
            _line_cut = false;

            if (!_head.status) {
                if (strncmp_P(_line, PSTR("HTTP/1."), 7)) {
                    finish(false, PSTR("no HTTP response"));
                    return;
                }
                const char* space = strchr(_line, ' ');
                _head.status = space ? number(space + 1) : 0;
                continue;
            }

            if (!_line[0]) {
                headDone();
                return;
            }

            // a longer line is skipped (a Location that long is no redirect this client follows)
            if (!whole) {
                continue;
            }

            if (!strncasecmp_P(_line, PSTR("content-length:"), 15)) {
                _head.length = number(_line + 15);
            } else if (!strncasecmp_P(_line, PSTR("content-range:"), 14)) {
                const char* bytes = strstr_P(_line + 14, PSTR("bytes "));
                _head.range = bytes ? number(bytes + 6) : -1;
            } else if (!strncasecmp_P(_line, PSTR("location:"), 9)) {
                const char* value = _line + 9;
                while (*value == ' ') {
                    ++value;
                }
                copy(_head.location, sizeof(_head.location), value);
            } else if (!strncasecmp_P(_line, PSTR("transfer-encoding:"), 18)) {
                _head.chunked = strstr_P(_line + 18, PSTR("chunked")) != nullptr;
                _head.length = -1;
            }
        }
    }

    void headDone() {
        if (!post() && (_head.status >= 300) && (_head.status < 400) && _head.location[0] && (_redirects < 4)) {
            char next[UrlMax];
            if (!redirect(_target, _head.location, next, sizeof(next))) {
                finish(false, PSTR("bad redirect"));
                return;
            }
            ++_redirects;
            copy(_url, sizeof(_url), next);
            close();
            connect();
            return;
        }

        _result.status = _head.status;
        const char* error = _sink->begin(_head);
        if (error) {
            finish(false, error);
            return;
        }

        _state = State::Body;
        _last = now();
        if (!_head.chunked && (_head.length == 0)) {
            bodyEnd();
        }
    }

    // the body: plain (Content-Length or until closed) or chunked
    void stepBody() {
        uint8_t buffer[512];
        for (;;) {
            size_t room = std::min(sizeof(buffer), _sink->writable());
            if (!room) {
                return;
            }

            if (_head.chunked) {
                if (_chunk_state != ChunkState::Data) {
                    if (!chunkHead()) {
                        return;
                    }
                    continue;
                }
                room = std::min(room, _chunk_left);
            } else if (_head.length >= 0) {
                room = std::min(room, static_cast<size_t>(_head.length) - _body_done);
            }

            const auto got = appRead(buffer, room);
            if (!got) {
                if (drained()) {
                    if (!_head.chunked && (_head.length < 0)) {
                        bodyEnd();
                    } else {
                        finishf(false, PSTR("connection closed after %u bytes"), static_cast<unsigned>(_body_done));
                    }
                }
                return;
            }

            br_sha256_update(&_sha, buffer, got);
            _body_done += got;
            if (_head.chunked) {
                _chunk_left -= got;
                if (!_chunk_left) {
                    _chunk_state = ChunkState::DataEnd;
                }
            }

            const char* error = _sink->write(buffer, got);
            if (error) {
                finish(false, error);
                return;
            }

            if (!_head.chunked && (_head.length >= 0) && (_body_done >= static_cast<size_t>(_head.length))) {
                bodyEnd();
                return;
            }
        }
    }

    enum class ChunkState {
        Size,       // the hex size line
        Data,
        DataEnd,    // CR LF after the data
        Trailer,    // after the last (0) chunk
    };

    // the size line of the next chunk, or the end. false - not complete yet
    bool chunkHead() {
        for (;;) {
            uint8_t c;
            if (!appRead(&c, 1)) {
                if (drained()) {
                    finish(false, PSTR("chunked body cut"));
                }
                return false;
            }

            if (c != '\n') {
                if (_head_len + 1 < sizeof(_line)) {
                    _line[_head_len++] = static_cast<char>(c);
                }
                continue;
            }

            if (_head_len && (_line[_head_len - 1] == '\r')) {
                --_head_len;
            }
            _line[_head_len] = '\0';
            _head_len = 0;

            switch (_chunk_state) {
            case ChunkState::DataEnd:
                _chunk_state = ChunkState::Size;
                continue;
            case ChunkState::Size:
                _chunk_left = hexNumber(_line);
                if (!_chunk_left) {
                    _chunk_state = ChunkState::Trailer;
                    continue;
                }
                _chunk_state = ChunkState::Data;
                return true;
            case ChunkState::Trailer:
                if (!_line[0]) {
                    bodyEnd();
                    return false;
                }
                continue;
            case ChunkState::Data:
                return true;
            }
        }
    }

    void bodyEnd() {
        if (_sha256[0]) {
            uint8_t digest[br_sha256_SIZE];
            br_sha256_out(&_sha, digest);
            char text[sizeof(digest) * 2 + 1];
            hex(digest, sizeof(digest), text);
            if (strcasecmp(text, _sha256) != 0) {
                finish(false, PSTR("SHA-256 mismatch"));
                return;
            }
        }

        const char* error = _sink->end();
        if (error) {
            finish(false, error);
            return;
        }

        _result.length = _body_done;
        if (_sha256[0]) {
            finishf(true, PSTR("downloaded %u bytes, SHA-256 checked"), static_cast<unsigned>(_body_done));
        } else {
            finishf(true, PSTR("received %u bytes"), static_cast<unsigned>(_body_done));
        }
    }

    void finishf(bool ok, const char* format, ...) {
        char message[MessageMax];
        va_list args;
        va_start(args, format);
        vsnprintf_P(message, sizeof(message), format, args);
        va_end(args);
        finishWith(ok, message);
    }

    // `message`: a PSTR or a RAM string
    void finish(bool ok, const char* message) {
        char text[MessageMax];
        strncpy_P(text, message, sizeof(text) - 1);
        text[sizeof(text) - 1] = '\0';
        finishWith(ok, text);
    }

    void finishWith(bool ok, const char* message) {
        close();
        _state = State::Idle;
        _body.reset();
        _result.ok = ok;
        copy(_result.message, sizeof(_result.message), message);
        NET_LOG("%s", _result.message);
        if (_done) {
            auto done = std::move(_done);
            _done = nullptr;
            done(_result);
        }
    }

    void close() {
        _stream.close();
        _request.reset();
    }

    State _state { State::Idle };
    char _url[UrlMax] {};
    char _sha256[65] {};
    char _method[8] {};
    char _type[64] {};
    char _headers[256] {};
    std::unique_ptr<uint8_t[]> _body;
    size_t _body_len { 0 };
    uint32_t _from { 0 };
    Sink* _sink { nullptr };
    Done _done;
    Result _result;
    Target _target;
    int _redirects { 0 };
    uint32_t _last { 0 };

    tls::Stream _stream;

    std::unique_ptr<uint8_t[]> _request;
    size_t _request_len { 0 };
    size_t _request_sent { 0 };

    Head _head;
    char _line[256];
    size_t _head_len { 0 };
    bool _line_cut { false };

    ChunkState _chunk_state { ChunkState::Size };
    size_t _chunk_left { 0 };
    size_t _body_done { 0 };
    br_sha256_context _sha;
};

// the state (~1.5 KB) only once a request was made: the client objects of the modules cost nothing until then
Client::Client() = default;

Client::~Client() = default;

bool Client::start(const Request& request, Sink& sink, Done done) {
    if (!_impl) {
        _impl.reset(new (std::nothrow) Impl());
        if (!_impl) {
            return false;
        }
    }

    return _impl->start(request, sink, std::move(done));
}

bool Client::busy() const {
    return _impl && _impl->busy();
}

void Client::abort() {
    if (_impl) {
        _impl->abort();
    }
}

void Client::loop() {
    if (_impl) {
        _impl->loop();
    }
}

} // namespace http
} // namespace espurna
