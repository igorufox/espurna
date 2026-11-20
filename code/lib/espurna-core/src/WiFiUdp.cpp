#include "WiFiUdp.h"

#include <new>

#include <stdlib.h>
#include <string.h>

WiFiUDP::~WiFiUDP() {
    stop();
    delete _tx;
}

void WiFiUDP::receive(espurna::udp::Packet& packet) {
    const auto head = _head.load(std::memory_order_relaxed);
    const auto next = static_cast<uint8_t>((head + 1) % Queue);
    if (next == _tail.load(std::memory_order_acquire)) {
        return;
    }

    const size_t size = (packet.length() < BufferSize) ? packet.length() : BufferSize;
    auto* out = static_cast<Packet*>(malloc(sizeof(Packet) + size));
    if (!out) {
        return;
    }

    out->ip = packet.remoteIP();
    out->port = packet.remotePort();
    out->size = static_cast<uint16_t>(size);
    memcpy(out->data, packet.data(), size);

    _queue[head] = out;
    _head.store(next, std::memory_order_release);
}

bool WiFiUDP::listen(const IPAddress* address, uint16_t port) {
    stop();
    _udp.onPacket([this](espurna::udp::Packet& packet) {
        receive(packet);
    });

    return address
        ? _udp.listen(static_cast<uint32_t>(*address), port)
        : _udp.listen(port);
}

uint8_t WiFiUDP::begin(uint16_t port) {
    return listen(nullptr, port) ? 1 : 0;
}

uint8_t WiFiUDP::begin(IPAddress address, uint16_t port) {
    return listen(&address, port) ? 1 : 0;
}

uint8_t WiFiUDP::beginMulticast(IPAddress group, uint16_t port) {
    if (!listen(nullptr, port)) {
        return 0;
    }

    if (!_udp.joinGroup(static_cast<uint32_t>(group))) {
        stop();
        return 0;
    }

    _multicast = group;
    _joined = true;
    return 1;
}

void WiFiUDP::drop() {
    free(_rx);
    _rx = nullptr;
    _rx_index = 0;
}

void WiFiUDP::stop() {
    if (_joined) {
        _udp.leaveGroup(static_cast<uint32_t>(_multicast));
        _joined = false;
    }

    // closed in lwIP's context: no receive() after this
    _udp.close();

    auto tail = _tail.load(std::memory_order_relaxed);
    while (tail != _head.load(std::memory_order_acquire)) {
        free(_queue[tail]);
        _queue[tail] = nullptr;
        tail = static_cast<uint8_t>((tail + 1) % Queue);
    }
    _tail.store(tail, std::memory_order_release);

    drop();
}

int WiFiUDP::beginPacket(IPAddress ip, uint16_t port) {
    if (!_tx) {
        _tx = new (std::nothrow) espurna::udp::Message(BufferSize);
    }

    if (!_tx || !*_tx) {
        return 0;
    }

    // sending only (the debug output): any local port
    if (!_udp.connected() && !listen(nullptr, 0)) {
        return 0;
    }

    // a new message, what was not sent is dropped
    _tx->clear();
    _tx_ip = ip;
    _tx_port = port;
    return 1;
}

int WiFiUDP::beginPacket(const char* host, uint16_t port) {
    uint32_t address;
    if (!espurna::udp::Socket::resolve(host, address)) {
        return 0;
    }

    return beginPacket(IPAddress(address), port);
}

int WiFiUDP::endPacket() {
    if (!_tx || !_tx_port) {
        return 0;
    }

    const auto sent = _udp.sendTo(*_tx, static_cast<uint32_t>(_tx_ip), _tx_port);
    _tx_port = 0;

    return (sent > 0) ? 1 : 0;
}

size_t WiFiUDP::write(uint8_t value) {
    return write(&value, 1);
}

size_t WiFiUDP::write(const uint8_t* data, size_t size) {
    if (!_tx || !_tx_port) {
        return 0;
    }

    return _tx->write(data, size);
}

int WiFiUDP::parsePacket() {
    drop();

    const auto tail = _tail.load(std::memory_order_relaxed);
    if (tail == _head.load(std::memory_order_acquire)) {
        return 0;
    }

    _rx = _queue[tail];
    _queue[tail] = nullptr;
    _tail.store(static_cast<uint8_t>((tail + 1) % Queue), std::memory_order_release);

    _remote_ip = IPAddress(_rx->ip);
    _remote_port = _rx->port;

    return static_cast<int>(_rx->size);
}

int WiFiUDP::available() {
    return _rx
        ? static_cast<int>(_rx->size - _rx_index)
        : 0;
}

int WiFiUDP::read() {
    return (_rx && (_rx_index < _rx->size))
        ? _rx->data[_rx_index++]
        : -1;
}

int WiFiUDP::read(unsigned char* buffer, size_t len) {
    const size_t left = available();
    const size_t out = (len < left) ? len : left;
    if (out) {
        memcpy(buffer, _rx->data + _rx_index, out);
        _rx_index += out;
    }

    return static_cast<int>(out);
}

int WiFiUDP::peek() {
    return (_rx && (_rx_index < _rx->size))
        ? _rx->data[_rx_index]
        : -1;
}

void WiFiUDP::flush() {
    drop();
}
