/*

TLS - the one TLS of ESPurna (the head of every chip), under the HTTP client and the MQTT client: a byte stream over the
TCP connection of libs/tcp.h, plain or TLS 1.2 by mbedtls (owner 2026-11-18: one crypto, mbedtls on every chip).

- the head has the TCP connection and the plain stream (libs/tls_impl.h); a TLS session - the handshake, the records,
  the server's certificate - is the tls module's (module_tls.cpp, libs/tls_mbedtls.h: mbedtls of the chip's SDK),
  asked for through one hook (tls::open(), net.cpp). Without the module a secure connection fails with
  Failure::NoTls, plain ones work
- the session does its I/O on the TCP connection from the loop (never in lwIP's context, which would stop the network
  for the length of a handshake)
- the server: not checked (a download is checked against its SHA-256 instead), the SHA-1 fingerprint of its
  certificate (MQTT `mqttFP`), or its chain against CA certificates (MQTT `mqttCA`, else the built-in root)
- the records: by default every connection asks for small ones by itself (max_fragment_length): 512, then 4096,
  then full ones, what the server took kept per host + port until the reboot (libs/tls_impl.h, AutoSizes); or one
  size asked for (Options::fragment 512 .. 4096), and a server that ignores it fails the connection

    espurna::tls::Stream stream;
    stream.connect("host", 443, options);
    ... from the loop: stream.ready() / read() / write() / drained() ...

*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

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

    // 512, 1024, 2048 or 4096: records this small (the input buffer is that big); 0 - auto: the smallest the server
    // takes (512, 4096 or full, found by the first connection to it)
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

// the hardware random of the chip (libs/tls_impl.h)
void random(uint8_t* out, size_t len);

// the error of a connection that got a record bigger than its input buffer (the server ignored max_fragment_length):
// the caller's retry gets full records (libs/tls_impl.h AutoSizes)
bool recordTooLarge(int error);

// --- the session (the tls module) -----------------------------------------------------------------------------------

// The TLS of one connection over the TCP connection of the stream: mbedtls in the tls module (libs/tls_mbedtls.h).
// Every call returns at once; the errors are mbedtls' (tlsError() of the stream)
class Session {
public:
    virtual ~Session() = default;

    // 1 - done, 0 - not yet, < 0 - the error
    virtual int handshake() = 0;
    // > 0 bytes, 0 - nothing now, < 0 - closed by the peer (0x7880 close notify too) or the error
    virtual int read(uint8_t* out, size_t len) = 0;
    // >= 0 bytes taken, < 0 - the error
    virtual int write(const uint8_t* data, size_t len) = 0;
    // close notify to the peer (best effort)
    virtual void close() = 0;
    // after a failed handshake: what the certificate check said (Failure::Fingerprint / Untrusted / None)
    virtual Failure verify() const = 0;
};

} // namespace tls

namespace tcp {
class Connection;
} // namespace tcp

namespace tls {

// The session of a secure connection, `tcp` connected: the tls module's (net.cpp asks for it through the module API).
// nullptr - `failure` says why (Failure::NoTls: no tls module)
Session* open(tcp::Connection& tcp, const char* host, const Options& options, Failure& failure, int& error);

} // namespace tls
} // namespace espurna
