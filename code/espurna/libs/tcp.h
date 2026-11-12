/*

TCP - the one TCP connection of everything ESPurna builds (the head of every chip, code/safeboot-app, code/migration-app),
under the HTTP client, the HTTP server, telnet. No Arduino: lwIP's raw TCP API, the same on every chip.

- lwIP's callbacks only copy what arrived into a ring buffer and note that the peer closed. The TCP window stays
  closed until the loop took the bytes (tcp_recved() then); a segment that does not fit is refused, lwIP gives it again
- the pcb is touched only in lwIP's context: directly without an OS (ESP8266 NONOS, NO_SYS), else through
  tcpip_api_call()
- the loop reads and writes, never waiting

The implementation is libs/net_impl.h, included by one .cpp of each image.

*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace espurna {
namespace tcp {

// ms, lwIP's clock
uint32_t now();

class Connection {
public:
    Connection();
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // a client: resolves `host` and connects. false - it could not even start (no memory, no DNS slot)
    bool connect(const char* host, uint16_t port);

    bool connected() const;     // the handshake is done
    bool closed() const;        // the peer closed, the connection broke, or it never connected
    bool resolveFailed() const; // closed() because the name was not resolved

    // received bytes the loop did not take yet; read() opens the window again
    size_t available();
    size_t read(uint8_t* out, size_t len);

    // what write() takes now; write() copies (nothing of `data` is kept) and sends
    size_t writable();
    size_t write(const uint8_t* data, size_t len);

    // the peer's and our own address of this connection, IPv4 in network order (0 when not IPv4 or not connected)
    uint32_t remoteIp() const;
    uint32_t localIp() const;

    void close();

    class Impl;

private:
    friend class Listener;
    std::unique_ptr<Impl> _impl;
};

// a TCP server port: connections are accepted in lwIP's context and wait for the loop
class Listener {
public:
    Listener();
    ~Listener();

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    // up to `pending` accepted connections wait for accept(); more are refused
    bool begin(uint16_t port, size_t pending = 4);
    void end();

    // the next accepted connection, nullptr when none
    std::unique_ptr<Connection> accept();

    class Impl;

private:
    std::unique_ptr<Impl> _impl;
};

} // namespace tcp
} // namespace espurna
