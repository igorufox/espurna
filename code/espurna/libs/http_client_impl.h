/*

HTTP CLIENT - the implementation (libs/http_client.h), included by exactly one .cpp of an image: ESPurna's
net.cpp (the head, through libs/net_impl.h), code/mini-app's and code/safeboot-app's main.cpp. No Arduino here: lwIP, BearSSL and the C
library, the same on every chip. What that .cpp defines before the include is only how the image logs and locks,
never what the client does (libs/net_impl.h lists them). The connection is libs/tcp.h

*/

#pragma once

#include "http_client.h"
#include "tcp.h"
#include "url_parse.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <mutex>
#include <new>
#include <type_traits>

#if defined(ESP8266)
#include <pgmspace.h>
#include <StackThunk.h>
#elif defined(ESP32) || defined(ESP_PLATFORM)
#include <esp_random.h>
#endif

// the flash string calls of the ESP8266; plain ones elsewhere (ESP32 and Beken Arduino define them already)
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef PSTR
#define PSTR(s) (s)
#endif
#ifndef memcpy_P
#define memcpy_P memcpy
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

#if defined(LIBRETINY)
extern "C" void lt_rand_bytes(uint8_t* buf, size_t len);
#endif

// --- what the chip forces --------------------------------------------------------------------------------------------

namespace espurna {
namespace http {
namespace chip {

// entropy for the TLS engine
inline void random(uint8_t* out, size_t len) {
#if defined(ESP8266)
    // the hardware random number register (what ESP.random() reads)
    while (len) {
        const uint32_t value = *reinterpret_cast<volatile uint32_t*>(0x3FF20E44);
        const size_t take = std::min<size_t>(len, sizeof(value));
        memcpy(out, &value, take);
        out += take;
        len -= take;
    }
#elif defined(ESP32) || defined(ESP_PLATFORM)
    esp_fill_random(out, len);
#elif defined(LIBRETINY)
    lt_rand_bytes(out, len);
#endif
}

} // namespace chip
} // namespace http
} // namespace espurna

#if defined(ESP8266)

// The engine steps that run the handshake and the record crypto go to the second stack of the core (StackThunk, 6 KB
// on the heap while a connection is open). Own thunks: the ones of BearSSLHelpers.cpp would link its RSA and EC
// verifiers
extern "C" {

void espurna_http_sendrec_ack(br_ssl_engine_context* cc, size_t len) {
    br_ssl_engine_sendrec_ack(cc, len);
}

void espurna_http_recvrec_ack(br_ssl_engine_context* cc, size_t len) {
    br_ssl_engine_recvrec_ack(cc, len);
}

void espurna_http_sendapp_ack(br_ssl_engine_context* cc, size_t len) {
    br_ssl_engine_sendapp_ack(cc, len);
}

void espurna_http_recvapp_ack(br_ssl_engine_context* cc, size_t len) {
    br_ssl_engine_recvapp_ack(cc, len);
}

void espurna_http_flush(br_ssl_engine_context* cc, int force) {
    br_ssl_engine_flush(cc, force);
}

void thunk_espurna_http_sendrec_ack(br_ssl_engine_context*, size_t);
void thunk_espurna_http_recvrec_ack(br_ssl_engine_context*, size_t);
void thunk_espurna_http_sendapp_ack(br_ssl_engine_context*, size_t);
void thunk_espurna_http_recvapp_ack(br_ssl_engine_context*, size_t);
void thunk_espurna_http_flush(br_ssl_engine_context*, int);

} // extern "C"

make_stack_thunk(espurna_http_sendrec_ack);
make_stack_thunk(espurna_http_recvrec_ack);
make_stack_thunk(espurna_http_sendapp_ack);
make_stack_thunk(espurna_http_recvapp_ack);
make_stack_thunk(espurna_http_flush);

#define HTTP_CLIENT_STEP(name) thunk_espurna_http_##name

#else

// the loop task has the stack for it
#define HTTP_CLIENT_STEP(name) br_ssl_engine_##name

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

// --- TLS: the server key without validating the chain -------------------------------------------------------------

// The server certificate (the first one) is kept. Its key: the tls module's X.509 decoder (RSA or EC) when installed,
// else the RSA key from SubjectPublicKeyInfo: the rsaEncryption OID, then BIT STRING { SEQUENCE { INTEGER n,
// INTEGER e } } (smaller than the decoder)
constexpr size_t CertMax { 4096 };

struct Insecure {
    const br_x509_class* vtable;
    uint8_t* cert;
    size_t size;
    size_t len;
    br_x509_pkey pkey;
    void* key;          // the tls module's block the key points into
};

bool tag(const uint8_t* der, size_t len, size_t& pos, uint8_t type, size_t& out) {
    if ((pos + 2 > len) || (der[pos] != type)) {
        return false;
    }

    out = der[pos + 1];
    pos += 2;
    if (out & 0x80) {
        size_t bytes = out & 0x7f;
        if ((bytes > 2) || (pos + bytes > len)) {
            return false;
        }

        out = 0;
        while (bytes--) {
            out = (out << 8) | der[pos++];
        }
    }

    return pos + out <= len;
}

bool rsaKey(const uint8_t* der, size_t len, br_rsa_public_key& out) {
    static const uint8_t Oid[] { 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };
    for (size_t pos = 0; pos + sizeof(Oid) < len; ++pos) {
        if (memcmp(der + pos, Oid, sizeof(Oid)) != 0) {
            continue;
        }

        pos += sizeof(Oid);
        if ((pos + 2 <= len) && (der[pos] == 0x05) && !der[pos + 1]) {
            pos += 2;
        }

        size_t size;
        if (!tag(der, len, pos, 0x03, size) || der[pos++] || !tag(der, len, pos, 0x30, size)
            || !tag(der, len, pos, 0x02, size))
        {
            return false;
        }

        out.n = const_cast<uint8_t*>(der + pos);
        out.nlen = size;
        pos += size;
        while (out.nlen && !*out.n) {
            ++out.n;
            --out.nlen;
        }

        if (!tag(der, len, pos, 0x02, size)) {
            return false;
        }

        out.e = const_cast<uint8_t*>(der + pos);
        out.elen = size;
        return out.nlen && size;
    }

    return false;
}

void insecureStartChain(const br_x509_class** ctx, const char*) {
    auto* self = reinterpret_cast<Insecure*>(ctx);
    self->size = 0;
    self->len = 0;
}

void insecureStartCert(const br_x509_class** ctx, uint32_t length) {
    auto* self = reinterpret_cast<Insecure*>(ctx);
    if (!self->size && !self->cert && (length <= CertMax)) {
        self->cert = static_cast<uint8_t*>(malloc(length));
        self->size = self->cert ? length : 0;
    }
}

void insecureAppend(const br_x509_class** ctx, const unsigned char* buf, size_t len) {
    auto* self = reinterpret_cast<Insecure*>(ctx);
    if (self->cert && (self->len + len <= self->size)) {
        memcpy(self->cert + self->len, buf, len);
        self->len += len;
    }
}

void insecureEndCert(const br_x509_class**) {
}

unsigned insecureEndChain(const br_x509_class** ctx) {
    auto* self = reinterpret_cast<Insecure*>(ctx);
    if (!self->cert || (self->len != self->size)) {
        return BR_ERR_X509_UNSUPPORTED;
    }

    free(self->key);
    self->key = tls::key(self->cert, self->len, &self->pkey);
    if (self->key) {
        return 0;
    }

    self->pkey.key_type = BR_KEYTYPE_RSA;
    return rsaKey(self->cert, self->len, self->pkey.key.rsa) ? 0 : BR_ERR_X509_UNSUPPORTED;
}

const br_x509_pkey* insecureGetPkey(const br_x509_class* const* ctx, unsigned* usages) {
    if (usages) {
        *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    }

    return &reinterpret_cast<const Insecure*>(ctx)->pkey;
}

const br_x509_class InsecureClass PROGMEM {
    sizeof(Insecure),
    insecureStartChain,
    insecureStartCert,
    insecureAppend,
    insecureEndCert,
    insecureEndChain,
    insecureGetPkey,
};

// the base ones; the tls module replaces the list with its own (these included)
const uint16_t Suites[] PROGMEM {
    BR_TLS_RSA_WITH_AES_128_GCM_SHA256,
};

// the engine (~4 KB) and its record buffers (~17 KB) are two blocks, a fragmented heap has a better chance
struct Tls {
    ~Tls() {
        free(x509.cert);
        free(x509.key);
    }

    br_ssl_client_context sc;
    Insecure x509;
};

constexpr size_t InputSize { BR_SSL_BUFSIZE_INPUT };
constexpr size_t OutputSize { 837 };

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
        Connecting,
        Handshake,
        Request,
        Head,
        Body,
    };

    // --- the connection (libs/tcp.h) ------------------------------------------------------------------------------

    bool connect() {
        Target target;
        if (!parse(_url, target)) {
            finish(false, PSTR("invalid URL"));
            return false;
        }

        _target = target;
        _conn.reset(new (std::nothrow) tcp::Connection());
        if (!_conn) {
            finish(false, PSTR("out of memory"));
            return false;
        }

        _head_len = 0;
        _chunk_left = 0;
        _chunk_state = ChunkState::Size;
        _body_done = 0;
        _head = Head{};
        br_sha256_init(&_sha);

        NET_LOG("%s %s", _method, _url);

        _state = State::Connecting;
        _last = now();

        const bool started = _conn->connect(_target.host, _target.port);
        if (!started) {
            finishf(false, PSTR("cannot resolve %s"), _target.host);
            return false;
        }

        _state = State::Handshake;
        return true;
    }

    size_t ringAvailable() {
        return _conn ? _conn->available() : 0;
    }

    size_t ringRead(uint8_t* out, size_t len) {
        return _conn ? _conn->read(out, len) : 0;
    }

    size_t send(const uint8_t* data, size_t len) {
        return _conn ? _conn->write(data, len) : 0;
    }

    bool closed() const {
        return !_conn || _conn->closed();
    }

    // --- TLS or plain, the same calls -------------------------------------------------------------------------------

    bool tlsSetup() {
        if (!_target.https) {
            return true;
        }

        _tls.reset(new (std::nothrow) Tls());
        _buffers.reset(new (std::nothrow) unsigned char[InputSize + OutputSize]);
        if (!_tls || !_buffers) {
            finish(false, PSTR("out of memory for TLS"));
            return false;
        }

#if defined(ESP8266)
        stack_thunk_add_ref();
        _thunk = true;
#endif

        auto* eng = &_tls->sc.eng;
        br_ssl_client_zero(&_tls->sc);
        br_ssl_engine_set_versions(eng, BR_TLS12, BR_TLS12);

        uint16_t suites[sizeof(Suites) / sizeof(Suites[0])];
        memcpy_P(suites, Suites, sizeof(Suites));
        br_ssl_engine_set_suites(eng, suites, sizeof(suites) / sizeof(suites[0]));

        br_ssl_client_set_default_rsapub(&_tls->sc);
        br_ssl_engine_set_hash(eng, br_sha256_ID, &br_sha256_vtable);
        br_ssl_engine_set_prf_sha256(eng, &br_tls12_sha256_prf);

        // the smallest AES and GHASH of BearSSL (not constant-time, no secret worth a timing attack here)
        br_ssl_engine_set_gcm(eng, &br_sslrec_in_gcm_vtable, &br_sslrec_out_gcm_vtable);
        br_ssl_engine_set_aes_ctr(eng, &br_aes_small_ctr_vtable);
        br_ssl_engine_set_ghash(eng, &br_ghash_ctmul);

        // ESPurna's tls module, when installed: its suites (ECDHE, ECDSA, ChaCha20) and their algorithms
        tls::extend(&_tls->sc);

        _tls->x509.vtable = &InsecureClass;
        br_ssl_engine_set_x509(eng, &_tls->x509.vtable);
        br_ssl_engine_set_buffers_bidi(eng,
            _buffers.get(), InputSize,
            _buffers.get() + InputSize, OutputSize);

        uint8_t seed[32];
        chip::random(seed, sizeof(seed));
        br_ssl_engine_inject_entropy(eng, seed, sizeof(seed));

        if (!br_ssl_client_reset(&_tls->sc, _target.host, 0)) {
            tlsFailed(PSTR("TLS setup"));
            return false;
        }

        return true;
    }

    bool tls() const {
        return static_cast<bool>(_tls);
    }

    void tlsFailed(const char* what) {
        finishf(false, PSTR("%s failed, BearSSL error %d"), what,
            _tls ? br_ssl_engine_last_error(&_tls->sc.eng) : -1);
    }

    // records out and in, as far as it goes without waiting. false - the engine closed (error or not)
    bool tlsPump() {
        auto* eng = &_tls->sc.eng;
        for (;;) {
            const auto state = br_ssl_engine_current_state(eng);
            if (state & BR_SSL_CLOSED) {
                return false;
            }

            bool moved { false };
            if (state & BR_SSL_SENDREC) {
                size_t len;
                auto* buf = br_ssl_engine_sendrec_buf(eng, &len);
                const auto sent = send(buf, len);
                if (sent) {
                    HTTP_CLIENT_STEP(sendrec_ack)(eng, sent);
                    _last = now();
                    moved = true;
                }
            }

            if ((state & BR_SSL_RECVREC) && ringAvailable()) {
                size_t len;
                auto* buf = br_ssl_engine_recvrec_buf(eng, &len);
                const auto got = ringRead(buf, len);
                if (got) {
                    HTTP_CLIENT_STEP(recvrec_ack)(eng, got);
                    _last = now();
                    moved = true;
                }
            }

            if (!moved) {
                return true;
            }
        }
    }

    // application data out: as much as goes now
    size_t appWrite(const uint8_t* data, size_t len) {
        if (tls()) {
            auto* eng = &_tls->sc.eng;
            if (!tlsPump() || !(br_ssl_engine_current_state(eng) & BR_SSL_SENDAPP)) {
                return 0;
            }

            size_t avail;
            auto* buf = br_ssl_engine_sendapp_buf(eng, &avail);
            const size_t size = std::min(avail, len);
            memcpy(buf, data, size);
            HTTP_CLIENT_STEP(sendapp_ack)(eng, size);
            HTTP_CLIENT_STEP(flush)(eng, 0);
            tlsPump();
            return size;
        }

        const auto out = send(data, len);
        if (out) {
            _last = now();
        }
        return out;
    }

    // application data in: what is there now
    size_t appRead(uint8_t* out, size_t len) {
        if (tls()) {
            auto* eng = &_tls->sc.eng;
            tlsPump();
            if (!(br_ssl_engine_current_state(eng) & BR_SSL_RECVAPP)) {
                return 0;
            }

            size_t avail;
            auto* buf = br_ssl_engine_recvapp_buf(eng, &avail);
            const size_t size = std::min(avail, len);
            memcpy(out, buf, size);
            HTTP_CLIENT_STEP(recvapp_ack)(eng, size);
            return size;
        }

        const auto got = ringRead(out, len);
        if (got) {
            _last = now();
        }
        return got;
    }

    // nothing more will come: the server closed and everything it sent was taken
    bool drained() {
        if (tls()) {
            return closed() && !ringAvailable()
                && !(br_ssl_engine_current_state(&_tls->sc.eng) & BR_SSL_RECVAPP);
        }

        return closed() && !ringAvailable();
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
        case State::Connecting:
            break;
        }
    }

    bool post() const {
        return strcmp(_method, "POST") == 0;
    }

    // the request head and the body, in one block
    bool buildRequest() {
        char head[UrlMax + 256];
        int len = snprintf(head, sizeof(head), "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nConnection: close\r\n",
            _method, _target.path, _target.host, HTTP_CLIENT_AGENT);
        if (_from && (len > 0) && (static_cast<size_t>(len) < sizeof(head))) {
            len += snprintf(head + len, sizeof(head) - len, "Range: bytes=%u-\r\n", static_cast<unsigned>(_from));
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
        if (!_conn || !_conn->connected()) {
            if (closed()) {
                if (_conn && _conn->resolveFailed()) {
                    finishf(false, PSTR("cannot resolve %s"), _target.host);
                } else {
                    finishf(false, PSTR("cannot connect to %s"), _target.host);
                }
            }
            return;
        }

        if (!_handshake_started) {
            _handshake_started = true;
            if (!tlsSetup()) {
                return;
            }
        }

        if (tls()) {
            if (!tlsPump()) {
                tlsFailed(PSTR("TLS handshake"));
                return;
            }
            if (!(br_ssl_engine_current_state(&_tls->sc.eng) & BR_SSL_SENDAPP)) {
                if (closed() && !ringAvailable()) {
                    tlsFailed(PSTR("TLS handshake"));
                }
                return;
            }
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
        _conn.reset();

        _tls.reset();
        _buffers.reset();
#if defined(ESP8266)
        if (_thunk) {
            stack_thunk_del_ref();
            _thunk = false;
        }
#endif

        _request.reset();
        _handshake_started = false;
    }

    State _state { State::Idle };
    char _url[UrlMax] {};
    char _sha256[65] {};
    char _method[8] {};
    char _type[64] {};
    std::unique_ptr<uint8_t[]> _body;
    size_t _body_len { 0 };
    uint32_t _from { 0 };
    Sink* _sink { nullptr };
    Done _done;
    Result _result;
    Target _target;
    int _redirects { 0 };
    uint32_t _last { 0 };

    std::unique_ptr<tcp::Connection> _conn;
    bool _handshake_started { false };

    std::unique_ptr<Tls> _tls;
    std::unique_ptr<unsigned char[]> _buffers;
#if defined(ESP8266)
    bool _thunk { false };
#endif

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
