/*

UDP - the one UDP of everything ESPurna builds, on lwIP's raw API: espota (libs/espota.h), the debug syslog output,
SSDP, LLMNR, NetBIOS, the captive portal DNS (lib/DNSServer, the captive module), WiFiUDP of the module libraries. No
lwIP sockets anywhere (owner 2026-11-18: sockets, api_msg, netbuf out of the heads), no Arduino types: an IPv4 address
is a uint32_t in network order (lwIP's ip4_addr_t::addr), udp::ip4(a, b, c, d) makes one.

- packets are handled in lwIP's context, right in the receive callback (the first pbuf of a datagram)
- listen / close / sendTo / joinGroup from other tasks go through the tcpip thread (tcpip_api_call), directly in it
  (a reply from the receive callback) and without an OS (NO_SYS)

The implementation is libs/udp_impl.h, part of libs/net_impl.h (net.cpp of the head).

*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>

struct udp_pcb;

namespace espurna {
namespace udp {

constexpr uint32_t ip4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return static_cast<uint32_t>(a)
        | (static_cast<uint32_t>(b) << 8)
        | (static_cast<uint32_t>(c) << 16)
        | (static_cast<uint32_t>(d) << 24);
}

// a datagram being received: valid inside the receive callback only
class Packet {
public:
    Packet(uint8_t* data, size_t length, uint32_t ip, uint16_t port) :
        _data(data),
        _length(length),
        _ip(ip),
        _port(port)
    {}

    uint8_t* data() {
        return _data;
    }

    size_t length() const {
        return _length;
    }

    uint32_t remoteIP() const {
        return _ip;
    }

    uint16_t remotePort() const {
        return _port;
    }

private:
    uint8_t* _data;
    size_t _length;
    uint32_t _ip;
    uint16_t _port;
};

// a datagram being written, of a fixed size
class Message {
public:
    explicit Message(size_t size = 1460) :
        _buffer(static_cast<uint8_t*>(std::malloc(size))),
        _size(_buffer ? size : 0)
    {}

    ~Message() {
        std::free(_buffer);
    }

    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;

    size_t write(const uint8_t* data, size_t len) {
        const size_t left = space();
        if (len > left) {
            len = left;
        }

        if (len) {
            std::memcpy(_buffer + _index, data, len);
            _index += len;
        }

        return len;
    }

    size_t write(uint8_t data) {
        return write(&data, 1);
    }

    size_t space() const {
        return _size - _index;
    }

    uint8_t* data() {
        return _buffer;
    }

    size_t length() const {
        return _index;
    }

    // empty again, the buffer stays (the next message of the same sender)
    void clear() {
        _index = 0;
    }

    explicit operator bool() const {
        return _buffer != nullptr;
    }

private:
    uint8_t* _buffer;
    size_t _index { 0 };
    size_t _size;
};

class Socket {
public:
    using Handler = std::function<void(Packet&)>;

    Socket() = default;
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // set before listen(): the receive callback reads it in lwIP's context
    void onPacket(Handler handler) {
        _handler = std::move(handler);
    }

    // port 0: any free port (sending only)
    bool listen(uint16_t port);
    bool listen(uint32_t ip, uint16_t port);
    void close();

    // receive what is sent to the multicast group too (IGMP, every interface): SSDP, LLMNR, FauxmoESP
    bool joinGroup(uint32_t group);
    bool leaveGroup(uint32_t group);

    size_t sendTo(const uint8_t* data, size_t len, uint32_t ip, uint16_t port);
    size_t sendTo(Message& message, uint32_t ip, uint16_t port) {
        return sendTo(message.data(), message.length(), ip, port);
    }

    bool connected() const {
        return _pcb != nullptr;
    }

    // the address of `host` (a dotted address or a name for lwIP's DNS), waiting at most `timeout` ms. Not from
    // lwIP's own context
    static bool resolve(const char* host, uint32_t& out, uint32_t timeout = 5000);

    class Impl;

private:
    friend class Impl;

    udp_pcb* _pcb { nullptr };
    Handler _handler;
};

} // namespace udp
} // namespace espurna
