/*

TLS - the implementation (libs/tls.h), part of libs/net_impl.h. No Arduino: lwIP (libs/tcp.h), BearSSL and the C
library, the same on every chip

*/

#pragma once

#include "tls.h"
#include "tcp.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(ESP8266)
#include <pgmspace.h>
#include <StackThunk.h>
#elif defined(ESP32) || defined(ESP_PLATFORM)
#include <esp_random.h>
#endif

#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef memcpy_P
#define memcpy_P memcpy
#endif

#if defined(LIBRETINY)
extern "C" void lt_rand_bytes(uint8_t* buf, size_t len);
#endif

// --- what the chip forces --------------------------------------------------------------------------------------------

namespace espurna {
namespace tls {
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
} // namespace tls
} // namespace espurna

#if defined(ESP8266)

// The engine steps that run the handshake and the record crypto go to the second stack of the core (StackThunk, 6 KB
// on the heap while a connection is open). Own thunks: the ones of BearSSLHelpers.cpp would link its RSA and EC
// verifiers
extern "C" {

void espurna_tls_sendrec_ack(br_ssl_engine_context* cc, size_t len) {
    br_ssl_engine_sendrec_ack(cc, len);
}

void espurna_tls_recvrec_ack(br_ssl_engine_context* cc, size_t len) {
    br_ssl_engine_recvrec_ack(cc, len);
}

void espurna_tls_sendapp_ack(br_ssl_engine_context* cc, size_t len) {
    br_ssl_engine_sendapp_ack(cc, len);
}

void espurna_tls_recvapp_ack(br_ssl_engine_context* cc, size_t len) {
    br_ssl_engine_recvapp_ack(cc, len);
}

void espurna_tls_flush(br_ssl_engine_context* cc, int force) {
    br_ssl_engine_flush(cc, force);
}

// The long RSA / EC math of the BearSSL fork (src/inner.h, ec_prime_i15/i31.c, rsa_i15/i31_priv.c) calls this between
// its steps; the core of a newer Arduino ESP8266 defines it in StackThunk.cpp, core 3.1.2 does not (its own libbearssl.a
// called optimistic_yield(10000) at the same places). The same here: yields when the 10 ms of the loop are up. The tls
// module links against the head's one
void stack_thunk_yield() {
    optimistic_yield(10000);
}

void thunk_espurna_tls_sendrec_ack(br_ssl_engine_context*, size_t);
void thunk_espurna_tls_recvrec_ack(br_ssl_engine_context*, size_t);
void thunk_espurna_tls_sendapp_ack(br_ssl_engine_context*, size_t);
void thunk_espurna_tls_recvapp_ack(br_ssl_engine_context*, size_t);
void thunk_espurna_tls_flush(br_ssl_engine_context*, int);

} // extern "C"

make_stack_thunk(espurna_tls_sendrec_ack);
make_stack_thunk(espurna_tls_recvrec_ack);
make_stack_thunk(espurna_tls_sendapp_ack);
make_stack_thunk(espurna_tls_recvapp_ack);
make_stack_thunk(espurna_tls_flush);

#define ESPURNA_TLS_STEP(name) thunk_espurna_tls_##name

#else

// the loop task has the stack for it
#define ESPURNA_TLS_STEP(name) br_ssl_engine_##name

#endif

namespace espurna {
namespace tls {
namespace {

// --- the server key, without validating the chain; the fingerprint when asked for ---------------------------------

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

    bool first;         // the server certificate is being received
    bool check;
    uint8_t fingerprint[FingerprintSize];
    br_sha1_context sha;
    bool mismatch;
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
    self->first = true;
    self->mismatch = false;
}

void insecureStartCert(const br_x509_class** ctx, uint32_t length) {
    auto* self = reinterpret_cast<Insecure*>(ctx);
    if (!self->first) {
        return;
    }

    br_sha1_init(&self->sha);
    if (!self->cert && (length <= CertMax)) {
        self->cert = static_cast<uint8_t*>(malloc(length));
        self->size = self->cert ? length : 0;
    }
}

void insecureAppend(const br_x509_class** ctx, const unsigned char* buf, size_t len) {
    auto* self = reinterpret_cast<Insecure*>(ctx);
    if (!self->first) {
        return;
    }

    br_sha1_update(&self->sha, buf, len);
    if (self->cert && (self->len + len <= self->size)) {
        memcpy(self->cert + self->len, buf, len);
        self->len += len;
    }
}

void insecureEndCert(const br_x509_class** ctx) {
    auto* self = reinterpret_cast<Insecure*>(ctx);
    if (self->first && self->check) {
        uint8_t digest[br_sha1_SIZE];
        br_sha1_out(&self->sha, digest);
        self->mismatch = memcmp(digest, self->fingerprint, sizeof(digest)) != 0;
    }

    self->first = false;
}

unsigned insecureEndChain(const br_x509_class** ctx) {
    auto* self = reinterpret_cast<Insecure*>(ctx);
    if (self->mismatch) {
        return BR_ERR_X509_NOT_TRUSTED;
    }

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

// the engine (~4 KB) and its record buffers are two blocks, a fragmented heap has a better chance
struct Engine {
    ~Engine() {
        free(x509.cert);
        free(x509.key);
        free(ca);
    }

    br_ssl_client_context sc;
    Insecure x509;
    void* ca { nullptr };   // the block of the tls module's chain check
};

// a record: the fragment plus its header, MAC and padding (BR_SSL_BUFSIZE_INPUT holds a 16384 one)
constexpr size_t RecordOverhead { BR_SSL_BUFSIZE_INPUT - 16384 };
constexpr size_t OutputSize { 512 + RecordOverhead };

size_t inputSize(size_t fragment) {
    switch (fragment) {
    case 512:
    case 1024:
    case 2048:
    case 4096:
        return fragment + RecordOverhead;
    }

    return BR_SSL_BUFSIZE_INPUT;
}

int hexDigit(char c) {
    return ((c >= '0') && (c <= '9')) ? (c - '0') :
        ((c >= 'a') && (c <= 'f')) ? (c - 'a' + 10) :
        ((c >= 'A') && (c <= 'F')) ? (c - 'A' + 10) : -1;
}

void thunkRelease(bool& thunk) {
#if defined(ESP8266)
    if (thunk) {
        stack_thunk_del_ref();
    }
#endif
    thunk = false;
}

} // namespace

bool parseFingerprint(const char* text, uint8_t (&out)[FingerprintSize]) {
    size_t index = 0;
    while (text && *text) {
        if ((*text == ':') || (*text == ' ')) {
            ++text;
            continue;
        }

        const int high = hexDigit(text[0]);
        const int low = text[1] ? hexDigit(text[1]) : -1;
        if ((high < 0) || (low < 0) || (index >= FingerprintSize)) {
            return false;
        }

        out[index++] = static_cast<uint8_t>((high << 4) | low);
        text += 2;
    }

    return index == FingerprintSize;
}

// --- the stream ------------------------------------------------------------------------------------------------------

class Stream::Impl {
public:
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        close();
    }

    bool connect(const char* host, uint16_t port, const Options& options) {
        close();

        _options = options;
        _failure = Failure::None;
        _tls_error = 0;
        strncpy(_host, host ? host : "", sizeof(_host) - 1);
        _host[sizeof(_host) - 1] = '\0';

        _conn.reset(new (std::nothrow) tcp::Connection());
        if (!_conn) {
            fail(Failure::Memory);
            return false;
        }

        _state = State::Connecting;
        _last = tcp::now();
        if (!_conn->connect(_host, port)) {
            fail(Failure::Resolve);
            return false;
        }

        return true;
    }

    bool ready() {
        step();
        return _state == State::Ready;
    }

    bool pending() {
        step();
        return (_state == State::Connecting) || (_state == State::Handshake);
    }

    Failure failure() {
        step();
        return _failure;
    }

    int tlsError() const {
        return _tls_error;
    }

    size_t read(uint8_t* out, size_t len) {
        if (!ready()) {
            return 0;
        }

        if (_engine) {
            auto* eng = &_engine->sc.eng;
            pump();
            if (!(br_ssl_engine_current_state(eng) & BR_SSL_RECVAPP)) {
                return 0;
            }

            size_t avail;
            auto* buf = br_ssl_engine_recvapp_buf(eng, &avail);
            const size_t size = std::min(avail, len);
            memcpy(out, buf, size);
            ESPURNA_TLS_STEP(recvapp_ack)(eng, size);
            return size;
        }

        const auto got = _conn->read(out, len);
        if (got) {
            _last = tcp::now();
        }
        return got;
    }

    size_t write(const uint8_t* data, size_t len) {
        if (!ready()) {
            return 0;
        }

        if (_engine) {
            auto* eng = &_engine->sc.eng;
            if (!pump() || !(br_ssl_engine_current_state(eng) & BR_SSL_SENDAPP)) {
                return 0;
            }

            size_t avail;
            auto* buf = br_ssl_engine_sendapp_buf(eng, &avail);
            const size_t size = std::min(avail, len);
            memcpy(buf, data, size);
            ESPURNA_TLS_STEP(sendapp_ack)(eng, size);
            ESPURNA_TLS_STEP(flush)(eng, 0);
            pump();
            return size;
        }

        const auto out = _conn->write(data, len);
        if (out) {
            _last = tcp::now();
        }
        return out;
    }

    bool drained() {
        if (!_conn) {
            return true;
        }

        if (_engine) {
            const bool open = pump();
            const auto state = br_ssl_engine_current_state(&_engine->sc.eng);
            if (!open) {
                return !(state & BR_SSL_RECVAPP);
            }

            return _conn->closed() && !_conn->available() && !(state & BR_SSL_RECVAPP);
        }

        return _conn->closed() && !_conn->available();
    }

    uint32_t lastActivity() const {
        return _last;
    }

    uint32_t remoteIp() const {
        return _conn ? _conn->remoteIp() : 0;
    }

    uint32_t localIp() const {
        return _conn ? _conn->localIp() : 0;
    }

    void close() {
        release();
        _state = State::Idle;
    }

private:
    enum class State {
        Idle,
        Connecting,
        Handshake,
        Ready,
        Failed,
    };

    void release() {
        _conn.reset();
        _engine.reset();
        _buffers.reset();
        thunkRelease(_thunk);
    }

    void fail(Failure failure) {
        if (_engine) {
            _tls_error = br_ssl_engine_last_error(&_engine->sc.eng);
        }

        _failure = failure;
        _state = State::Failed;
        release();
    }

    void step() {
        switch (_state) {
        case State::Connecting:
            if (_conn->connected()) {
                _last = tcp::now();
                if (!_options.secure) {
                    _state = State::Ready;
                    return;
                }
                if (!setup()) {
                    return;
                }
                _state = State::Handshake;
                handshake();
            } else if (_conn->closed()) {
                fail(_conn->resolveFailed() ? Failure::Resolve : Failure::Connect);
            }
            break;

        case State::Handshake:
            handshake();
            break;

        case State::Idle:
        case State::Ready:
        case State::Failed:
            break;
        }
    }

    bool setup() {
        _input_size = inputSize(_options.fragment);
        _engine.reset(new (std::nothrow) Engine());
        _buffers.reset(new (std::nothrow) unsigned char[_input_size + OutputSize]);
        if (!_engine || !_buffers) {
            fail(Failure::Memory);
            return false;
        }

#if defined(ESP8266)
        stack_thunk_add_ref();
        _thunk = true;
#endif

        auto* eng = &_engine->sc.eng;
        br_ssl_client_zero(&_engine->sc);
        br_ssl_engine_set_versions(eng, BR_TLS12, BR_TLS12);

        uint16_t suites[sizeof(Suites) / sizeof(Suites[0])];
        memcpy_P(suites, Suites, sizeof(Suites));
        br_ssl_engine_set_suites(eng, suites, sizeof(suites) / sizeof(suites[0]));

        br_ssl_client_set_default_rsapub(&_engine->sc);
        br_ssl_engine_set_hash(eng, br_sha256_ID, &br_sha256_vtable);
        br_ssl_engine_set_prf_sha256(eng, &br_tls12_sha256_prf);

        // the smallest AES and GHASH of BearSSL (not constant-time, no secret worth a timing attack here)
        br_ssl_engine_set_gcm(eng, &br_sslrec_in_gcm_vtable, &br_sslrec_out_gcm_vtable);
        br_ssl_engine_set_aes_ctr(eng, &br_aes_small_ctr_vtable);
        br_ssl_engine_set_ghash(eng, &br_ghash_ctmul);

        // ESPurna's tls module, when installed: its suites (ECDHE, ECDSA, ChaCha20) and their algorithms
        tls::extend(&_engine->sc);

        if (_options.ca) {
            // the chain against trust anchors, by the tls module
            auto* check = tls::caCheck(_options.ca_pem, &_engine->ca);
            if (!check) {
                fail(Failure::NoCa);
                return false;
            }
            br_ssl_engine_set_x509(eng, check);
        } else {
            auto& x509 = _engine->x509;
            x509.vtable = &InsecureClass;
            x509.check = _options.check;
            memcpy(x509.fingerprint, _options.fingerprint, sizeof(x509.fingerprint));
            br_ssl_engine_set_x509(eng, &x509.vtable);
        }

        // an input buffer below the full record size makes the client ask for max_fragment_length
        br_ssl_engine_set_buffers_bidi(eng,
            _buffers.get(), _input_size,
            _buffers.get() + _input_size, OutputSize);

        uint8_t seed[32];
        chip::random(seed, sizeof(seed));
        br_ssl_engine_inject_entropy(eng, seed, sizeof(seed));

        if (!br_ssl_client_reset(&_engine->sc, _host, 0)) {
            fail(Failure::Tls);
            return false;
        }

        return true;
    }

    void handshake() {
        if (!pump()) {
            const int error = _engine ? br_ssl_engine_last_error(&_engine->sc.eng) : 0;
            const bool x509 = (error >= BR_ERR_X509_OK) && (error < BR_ERR_RECV_FATAL_ALERT);
            fail(!x509 ? Failure::Tls
                : _options.ca ? Failure::Untrusted
                : (error == BR_ERR_X509_NOT_TRUSTED) ? Failure::Fingerprint
                : Failure::Tls);
            return;
        }

        if (br_ssl_engine_current_state(&_engine->sc.eng) & BR_SSL_SENDAPP) {
            _state = State::Ready;
            return;
        }

        if (_conn->closed() && !_conn->available()) {
            fail(Failure::Tls);
        }
    }

    // records out and in, as far as it goes without waiting. false - the engine closed (error or not)
    bool pump() {
        auto* eng = &_engine->sc.eng;
        for (;;) {
            const auto state = br_ssl_engine_current_state(eng);
            if (state & BR_SSL_CLOSED) {
                return false;
            }

            bool moved { false };
            if (state & BR_SSL_SENDREC) {
                size_t len;
                auto* buf = br_ssl_engine_sendrec_buf(eng, &len);
                const auto sent = _conn->write(buf, len);
                if (sent) {
                    ESPURNA_TLS_STEP(sendrec_ack)(eng, sent);
                    _last = tcp::now();
                    moved = true;
                }
            }

            if ((state & BR_SSL_RECVREC) && _conn->available()) {
                size_t len;
                auto* buf = br_ssl_engine_recvrec_buf(eng, &len);
                const auto got = _conn->read(buf, len);
                if (got) {
                    ESPURNA_TLS_STEP(recvrec_ack)(eng, got);
                    _last = tcp::now();
                    moved = true;
                }
            }

            if (!moved) {
                return true;
            }
        }
    }

    State _state { State::Idle };
    Failure _failure { Failure::None };
    int _tls_error { 0 };
    Options _options;
    char _host[64] {};
    uint32_t _last { 0 };

    std::unique_ptr<tcp::Connection> _conn;
    std::unique_ptr<Engine> _engine;
    std::unique_ptr<unsigned char[]> _buffers;
    size_t _input_size { 0 };
    bool _thunk { false };
};

// the state only once a connection was made
Stream::Stream() = default;

Stream::~Stream() = default;

bool Stream::connect(const char* host, uint16_t port, const Options& options) {
    if (!_impl) {
        _impl.reset(new (std::nothrow) Impl());
        if (!_impl) {
            return false;
        }
    }

    return _impl->connect(host, port, options);
}

bool Stream::ready() {
    return _impl && _impl->ready();
}

bool Stream::pending() {
    return _impl && _impl->pending();
}

Failure Stream::failure() {
    return _impl ? _impl->failure() : Failure::None;
}

int Stream::tlsError() const {
    return _impl ? _impl->tlsError() : 0;
}

size_t Stream::read(uint8_t* out, size_t len) {
    return _impl ? _impl->read(out, len) : 0;
}

size_t Stream::write(const uint8_t* data, size_t len) {
    return _impl ? _impl->write(data, len) : 0;
}

bool Stream::drained() {
    return !_impl || _impl->drained();
}

uint32_t Stream::lastActivity() const {
    return _impl ? _impl->lastActivity() : 0;
}

uint32_t Stream::remoteIp() const {
    return _impl ? _impl->remoteIp() : 0;
}

uint32_t Stream::localIp() const {
    return _impl ? _impl->localIp() : 0;
}

void Stream::close() {
    if (_impl) {
        _impl->close();
    }
}

} // namespace tls
} // namespace espurna
