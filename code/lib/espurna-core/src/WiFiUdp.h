#pragma once
// WiFiUDP of the layer on espurna::udp::Socket of the head (code/espurna/libs/udp.h, no lwIP sockets): FauxmoESP
// (SSDP discovery, multicast). What arrives is copied in lwIP's context into a queue of Queue packets (more are
// dropped, as a full socket would), parsePacket() takes them in the loop

#include <Arduino.h>
#include "arduino-api/Udp.h"
#include "../../../espurna/libs/udp.h"

#include <atomic>

class WiFiUDP : public arduino::UDP {
public:
    static constexpr size_t BufferSize { 1460 };
    static constexpr size_t Queue { 4 };

    WiFiUDP() = default;
    ~WiFiUDP();

    WiFiUDP(const WiFiUDP&) = delete;
    WiFiUDP& operator=(const WiFiUDP&) = delete;

    uint8_t begin(uint16_t port) override;
    uint8_t begin(IPAddress address, uint16_t port);
    uint8_t beginMulticast(IPAddress group, uint16_t port) override;
    // the esp8266 core's form (the interface address first), the libraries' ESP8266 branch: every interface joins
    uint8_t beginMulticast(IPAddress, IPAddress group, uint16_t port) {
        return beginMulticast(group, port);
    }
    void stop() override;

    int beginPacket(IPAddress ip, uint16_t port) override;
    int beginPacket(const char* host, uint16_t port) override;
    int endPacket() override;
    size_t write(uint8_t value) override;
    size_t write(const uint8_t* buffer, size_t size) override;
    using Print::write;

    int parsePacket() override;
    int available() override;
    int read() override;
    int read(unsigned char* buffer, size_t len) override;
    int read(char* buffer, size_t len) override {
        return read(reinterpret_cast<unsigned char*>(buffer), len);
    }
    int peek() override;
    void flush() override;

    IPAddress remoteIP() override {
        return _remote_ip;
    }

    uint16_t remotePort() override {
        return _remote_port;
    }

private:
    struct Packet {
        uint32_t ip;
        uint16_t port;
        uint16_t size;
        uint8_t data[1];
    };

    bool listen(const IPAddress* address, uint16_t port);
    void receive(espurna::udp::Packet& packet);
    void drop();

    espurna::udp::Socket _udp;
    IPAddress _multicast;
    bool _joined { false };

    // filled by lwIP's context (_head), taken by the loop (_tail)
    Packet* _queue[Queue] {};
    std::atomic<uint8_t> _head { 0 };
    std::atomic<uint8_t> _tail { 0 };

    espurna::udp::Message* _tx { nullptr };
    IPAddress _tx_ip;
    uint16_t _tx_port { 0 };

    Packet* _rx { nullptr };
    size_t _rx_index { 0 };

    IPAddress _remote_ip;
    uint16_t _remote_port { 0 };
};
