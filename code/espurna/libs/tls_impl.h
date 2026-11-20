/*

TLS - the stream (libs/tls.h), part of libs/net_impl.h: the TCP connection and the plain stream in the head; a secure
one gets its session from the tls module (tls::open(), mbedtls: libs/tls_mbedtls.h). No crypto here, no Arduino.

*/

#pragma once

#include "tls.h"
#include "tcp.h"

#include <cstring>
#include <new>

#if defined(ESP_PLATFORM) || defined(ESP8266)
// esp_fill_random(): ESP-IDF esp_random.h, ESP8266_RTOS_SDK esp_system.h
#if __has_include(<esp_random.h>)
#include <esp_random.h>
#else
#include <esp_system.h>
#endif
#elif defined(BK72XX)
#include "../arch/beken/beken_api.h"
#endif

namespace espurna {
namespace tls {
namespace {

// --- the record size per server (Options::fragment 0: auto) ----------------------------------------------------------
//
// Every TLS connection asks for small records (max_fragment_length) by itself (owner 2026-11-17: «его надо постоянно
// пытаться включать при tls»; the NSPanel's weather session took the heap down to ~120 B with a 16 KB input buffer).
// The first connection to a host is the probe: 512, then 4096 if the server refused it, then full records; what
// worked is kept per host + port until the reboot, so the next connections go straight to it. A refusal: the
// handshake failed or the server closed during it. A server that sends a bigger record later on is marked «full»
// and the caller's retry gets it

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

int hexDigit(char c) {
    return ((c >= '0') && (c <= '9')) ? (c - '0') :
        ((c >= 'a') && (c <= 'f')) ? (c - 'a' + 10) :
        ((c >= 'A') && (c <= 'F')) ? (c - 'A' + 10) : -1;
}

} // namespace

void random(uint8_t* out, size_t len) {
#if defined(ESP_PLATFORM) || defined(ESP8266)
    esp_fill_random(out, len);
#elif defined(BK72XX)
    ::espurna::beken::rand_bytes(out, len);
#endif
}

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

// mbedtls: MBEDTLS_ERR_SSL_INVALID_RECORD (a record over the input buffer), MBEDTLS_ERR_SSL_BAD_INPUT_DATA
bool recordTooLarge(int error) {
    return (error == -0x7200) || (error == -0x7100);
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

        if (_session) {
            const auto got = _session->read(out, len);
            if (got > 0) {
                _last = tcp::now();
                return static_cast<size_t>(got);
            }

            if (got < 0) {
                ended(got);
            }

            return 0;
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

        if (_session) {
            const auto sent = _session->write(data, len);
            if (sent > 0) {
                _last = tcp::now();
                return static_cast<size_t>(sent);
            }

            if (sent < 0) {
                ended(sent);
            }

            return 0;
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

        if (_session) {
            return _closed || (_conn->closed() && !_conn->available());
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
        if (_session) {
            _session->close();
        }

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
        delete _session;
        _session = nullptr;
        _conn.reset();
        _closed = false;
    }

    void fail(Failure failure) {
        _failure = failure;
        _state = State::Failed;
        release();
    }

    // the session closed after the handshake: the peer said so, or an error
    void ended(int error) {
        _closed = true;
        if (error != -0x7880) {
            _tls_error = error;
        }

        // the handshake went with small records, the data then came in a bigger one: the server takes none, the
        // next connection (the caller's retry) gets full records
        if (_auto && AutoSizes[_step] && recordTooLarge(error)) {
            _step = AutoSteps - 1;
            autoStore(_host, _port, _step);
        }
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

    // the session of the tls module, Failure::NoTls without it
    bool setup() {
        auto options = _options;
        if (_auto) {
            options.fragment = AutoSizes[_step];
        }

        Failure failure { Failure::None };
        int error { 0 };
        _session = tls::open(*_conn, _host, options, failure, error);
        if (!_session) {
            _tls_error = error;
            fail((failure != Failure::None) ? failure : Failure::Tls);
            return false;
        }

        return true;
    }

    void handshake() {
        const auto result = _session->handshake();
        if (result > 0) {
            _state = State::Ready;
            _last = tcp::now();
            if (_auto) {
                autoStore(_host, _port, _step);
            }
            return;
        }

        if (result == 0) {
            return;
        }

        _tls_error = result;
        const auto verified = _session->verify();
        if (verified != Failure::None) {
            fail(verified);
            return;
        }

        if (refused()) {
            return;
        }

        fail(Failure::Tls);
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
    // false - nothing bigger is left
    bool refused() {
        if (!_auto || ((static_cast<size_t>(_step) + 1) >= AutoSteps)) {
            return false;
        }

        release();
        ++_step;
        open();
        return true;
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
    bool _closed { false };

    std::unique_ptr<tcp::Connection> _conn;
    Session* _session { nullptr };
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
