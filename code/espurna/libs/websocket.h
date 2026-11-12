/*

WEBSOCKET - the one WebSocket server of ESPurna (RFC 6455), on the TCP connection of libs/tcp.h, every chip. The HTTP
server (libs/http_server.h) hands an `Upgrade: websocket` request over: accept() answers the handshake (SHA-1 of the
key, BearSSL) and takes the connection. The loop drives it, nothing waits:
- text messages out (unmasked frames, one frame per message), queued per client up to QueueMax bytes
- messages in: masked frames, fragments joined, up to MessageMax bytes (longer ones are dropped), text or binary
- ping answered with pong, close answered and the connection closed, idle clients pinged

The implementation is libs/websocket_impl.h, part of libs/net_impl.h.

*/

#pragma once

#include "http_server.h"
#include "tcp.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace espurna {
namespace websocket {

constexpr size_t MessageMax { 4096 };
constexpr size_t QueueMax { 16 * 1024 };

enum class Event {
    Connect,
    Disconnect,
    Message,    // a whole message (text: a terminating 0 after `len` bytes)
};

// `id` of the client, `data` / `len` of a message
using EventHandler = std::function<void(Event event, uint32_t id, uint32_t ip, const uint8_t* data, size_t len)>;

class Server {
public:
    Server();
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void onEvent(EventHandler handler);

    // the handshake answered, the connection taken; false - not a WebSocket request (400 is sent) or too many clients
    bool accept(http::server::Request& request);

    bool hasClient(uint32_t id) const;
    size_t count() const;

    // a client still has queued output (a reply the restart waits for)
    bool pending() const;

    // bytes a client takes now without queueing more than QueueMax
    size_t availableForWrite(uint32_t id) const;

    bool text(uint32_t id, const char* data, size_t len);
    void textAll(const char* data, size_t len);

    void close(uint32_t id);
    void closeAll();

    // from the loop
    void loop();

    size_t clientsMax() const;
    void clientsMax(size_t value);

    class Impl;

private:
    std::unique_ptr<Impl> _impl;
};

} // namespace websocket
} // namespace espurna
