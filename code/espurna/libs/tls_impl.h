/*

TLS - the implementation (libs/tls.h), part of libs/net_impl.h. No Arduino: lwIP (libs/tcp.h), BearSSL and the C
library, the same on every chip

*/

#pragma once

#include "tls.h"
#include "tls_engine.h"
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

// --- the record size per server (Options::fragment 0: auto) ----------------------------------------------------------
//
// Every TLS connection asks for small records (max_fragment_length) by itself (owner 2026-11-17: «его надо постоянно
// пытаться включать при tls»; the NSPanel's weather session took the heap down to ~120 B with a 16.7 KB input buffer).
// The first connection to a host is the probe: 512, then 4096 if the server refused it, then full records; what
// worked is kept per host + port until the reboot, so the next connections go straight to it. A refusal: the server
// sent a bigger record (BR_ERR_TOO_LARGE), echoed another size (BR_ERR_BAD_FRAGLEN), answered with an alert or closed
// during the handshake. A server that sends a bigger record later on is marked «full» and the caller's retry gets it

constexpr size_t AutoSizes[] { 512, 4096, 0 };
constexpr size_t AutoSteps { sizeof(AutoSizes) / sizeof(AutoSizes[0]) };

struct AutoEntry {
    uint32_t hash;
    uint16_t port;
    uint8_t step;       // the index into AutoSizes
    bool used;
};

// a fixed table, the oldest entry replaced: no heap
constexpr size_t AutoEntries { 8 };
AutoEntry autoTable[AutoEntries] {};
size_t autoNext { 0 };

uint32_t autoHash(const char* host) {
    uint32_t hash { 2166136261u };
    for (; host && *host; ++host) {
        const char c = ((*host >= 'A') && (*host <= 'Z')) ? (*host - 'A' + 'a') : *host;
        hash = (hash ^ static_cast<uint8_t>(c)) * 16777619u;
    }

    return hash;
}

AutoEntry* autoFind(const char* host, uint16_t port) {
    const auto hash = autoHash(host);
    for (auto& entry : autoTable) {
        if (entry.used && (entry.hash == hash) && (entry.port == port)) {
            return &entry;
        }
    }

    return nullptr;
}

// the first step to try for this server: the known one, else 512
uint8_t autoStep(const char* host, uint16_t port) {
    const auto* entry = autoFind(host, port);
    return entry ? entry->step : 0;
}

// the server took (or needs) this step; one log line when the table learns something new
void autoStore(const char* host, uint16_t port, uint8_t step) {
    auto* entry = autoFind(host, port);
    if (entry && (entry->step == step)) {
        return;
    }

    if (!entry) {
        entry = &autoTable[autoNext];
        autoNext = (autoNext + 1) % AutoEntries;
    }

    entry->hash = autoHash(host);
    entry->port = port;
    entry->step = step;
    entry->used = true;

    if (AutoSizes[step]) {
        NET_LOG("%s: TLS records of %u bytes", host, static_cast<unsigned>(AutoSizes[step]));
    } else {
        NET_LOG("%s: TLS records of full size", host);
    }
}

// the handshake failed the way a server that does not take this record size fails it
bool autoRefused(int error) {
    return (error == BR_ERR_OK)             // closed during the handshake
        || (error == BR_ERR_TOO_LARGE)
        || (error == BR_ERR_BAD_FRAGLEN)
        || ((error >= BR_ERR_RECV_FATAL_ALERT) && (error < BR_ERR_SEND_FATAL_ALERT));
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
        _port = port;

        // no record size asked for: the one this server takes (autoStep(), 512 for a new one)
        _auto = _options.secure && (_options.fragment == 0);
        _step = _auto ? autoStep(_host, _port) : 0;

        return open();
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
        _input_size = inputSize(_auto ? AutoSizes[_step] : _options.fragment);
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

        // the TLS of this connection: the tls module's (ESPurna's head has only the records), Failure::NoTls without it
        uint8_t seed[32];
        chip::random(seed, sizeof(seed));
        const auto failure = tls::start(*_engine, _host, _options, seed, sizeof(seed),
            _buffers.get(), _input_size, _buffers.get() + _input_size, OutputSize);
        if (failure != Failure::None) {
            fail(failure);
            return false;
        }

        return true;
    }

    void handshake() {
        if (!pump()) {
            const int error = _engine ? br_ssl_engine_last_error(&_engine->sc.eng) : 0;
            if (refused(error)) {
                return;
            }

            const bool x509 = (error >= BR_ERR_X509_OK) && (error < BR_ERR_RECV_FATAL_ALERT);
            fail(!x509 ? Failure::Tls
                : _options.ca ? Failure::Untrusted
                : (error == BR_ERR_X509_NOT_TRUSTED) ? Failure::Fingerprint
                : Failure::Tls);
            return;
        }

        if (br_ssl_engine_current_state(&_engine->sc.eng) & BR_SSL_SENDAPP) {
            _state = State::Ready;
            if (_auto) {
                autoStore(_host, _port, _step);
            }
            return;
        }

        if (_conn->closed() && !_conn->available()) {
            if (refused(br_ssl_engine_last_error(&_engine->sc.eng))) {
                return;
            }
            fail(Failure::Tls);
        }
    }

    // the TCP connection, then (step()) the handshake
    bool open() {
        _conn.reset(new (std::nothrow) tcp::Connection());
        if (!_conn) {
            fail(Failure::Memory);
            return false;
        }

        _state = State::Connecting;
        _last = tcp::now();
        if (!_conn->connect(_host, _port)) {
            fail(Failure::Resolve);
            return false;
        }

        return true;
    }

    // auto record size: the server did not take this one, a new connection asks for the next (512, 4096, full).
    // false - not that kind of failure, or nothing bigger is left
    bool refused(int error) {
        if (!_auto || ((static_cast<size_t>(_step) + 1) >= AutoSteps) || !autoRefused(error)) {
            return false;
        }

        release();
        ++_step;
        open();
        return true;
    }

    // records out and in, as far as it goes without waiting. false - the engine closed (error or not)
    bool pump() {
        auto* eng = &_engine->sc.eng;
        for (;;) {
            const auto state = br_ssl_engine_current_state(eng);
            if (state & BR_SSL_CLOSED) {
                // the handshake went with small records, the data then came in a bigger one: the server takes none,
                // the next connection (the caller's retry) gets full records
                if (_auto && (_state == State::Ready) && AutoSizes[_step]
                    && (br_ssl_engine_last_error(eng) == BR_ERR_TOO_LARGE))
                {
                    _step = AutoSteps - 1;
                    autoStore(_host, _port, _step);
                }
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
    uint16_t _port { 0 };
    bool _auto { false };   // Options::fragment 0: the record size of the table (AutoSizes[_step])
    uint8_t _step { 0 };
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
