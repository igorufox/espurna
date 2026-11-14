/*

TLS - the one TLS of everything ESPurna builds (the head of every chip, code/safeboot-app, code/migration-app), under the
HTTP client and the MQTT client: a byte stream over the TCP connection of libs/tcp.h, plain or TLS 1.2 by BearSSL.

- BearSSL's engine has no I/O of its own: the loop moves its records to and from the TCP connection and runs its
  crypto there (never in lwIP's context, which would stop the network for the length of a handshake). The ESP8266
  core has BearSSL, the others take the same sources as a library (igorufox/bearssl-esp8266)
- the TCP connection, the plain stream and the records of a TLS connection are here (ESPurna: the head); making a TLS
  connection - the client, the handshake, RSA, AES-GCM, the server's certificate - is libs/tls_setup.h: ESPurna's tls
  module (module_tls.cpp, with ECDHE, ECDSA, ChaCha20-Poly1305 and the X.509 decoder of libs/tls_full.h), the base
  alone in code/migration-app. Without it a secure connection fails with Failure::NoTls (libs/tls_engine.h start())
- the server: not checked (a download is checked against its SHA-256 instead), the SHA-1 fingerprint of its
  certificate (MQTT `mqttFP`), or its chain against trust anchors (MQTT `mqttCA`, else the built-in root; the tls
  module does it, the head has only the hook)
- the records: full ones (a 16 KB input buffer), or the server is asked for smaller ones (max_fragment_length:
  512 .. 4096); a server that ignores it fails the connection
- what differs per chip is only what the chip forces: the entropy, and on the ESP8266 the second stack for the crypto
  steps (its loop stack is 4 KB, RSA i15 alone takes ~4.5 KB)

    espurna::tls::Stream stream;
    stream.connect("host", 443, options);
    ... from the loop: stream.ready() / read() / write() / drained() ...

The implementation is libs/tls_impl.h, part of libs/net_impl.h; the setup of a TLS connection libs/tls_setup.h.

*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

// BearSSL: our fork (igorufox/bearssl-esp8266, code/libraries.ini `bearssl`) on every chip, the ESP8266 too (its
// core's prebuilt libbearssl.a is taken out of the link, scripts/pio_main.py)
#include <bearssl.h>

namespace espurna {
namespace tls {

// --- the stream ---------------------------------------------------------------------------------------------------

constexpr size_t FingerprintSize { 20 };

struct Options {
    bool secure { false };

    // the SHA-1 of the server certificate must be `fingerprint`
    bool check { false };
    uint8_t fingerprint[FingerprintSize] {};

    // the chain of the server against trust anchors (the tls module): `ca_pem` (kept by the caller until the
    // connection is up), nullptr or empty - the built-in root
    bool ca { false };
    const char* ca_pem { nullptr };

    // 512, 1024, 2048 or 4096: records this small (the input buffer is that big); 0 - full records
    size_t fragment { 0 };
};

enum class Failure : uint8_t {
    None,
    Memory,
    Resolve,
    Connect,
    Tls,
    Fingerprint,
    Untrusted,      // the chain did not lead to a trust anchor (or the clock is not set)
    NoCa,           // the CA check is not available: no tls module, or no certificate in the PEM
    NoTls,          // a secure connection, and the image has no TLS (ESPurna: the tls module is not installed)
};

class Stream {
public:
    Stream();
    ~Stream();

    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    // resolves `host`, connects, then the TLS handshake (secure). false - it could not even start
    bool connect(const char* host, uint16_t port, const Options& options);

    // connected, and the handshake done: data goes both ways
    bool ready();
    // neither ready nor failed yet
    bool pending();
    // why it never got ready
    Failure failure();
    int tlsError() const;

    // what arrived and can be taken now
    size_t read(uint8_t* out, size_t len);
    // as much as goes now
    size_t write(const uint8_t* data, size_t len);

    // the peer closed (or the connection broke) and everything it sent was read
    bool drained();

    // ms of the last bytes in or out (tcp::now())
    uint32_t lastActivity() const;

    uint32_t remoteIp() const;
    uint32_t localIp() const;

    void close();

    class Impl;

private:
    std::unique_ptr<Impl> _impl;
};

// "AA:BB:..." or "AABB..." (40 hex digits) into `out`. false - not a fingerprint
bool parseFingerprint(const char* text, uint8_t (&out)[FingerprintSize]);

} // namespace tls
} // namespace espurna
