#pragma once
// Asynchronous UDP of ESPurna, one implementation for every chip (ESP8266, ESP32, LibreTiny), on the lwIP raw API.
// The part of the arduino-esp32 AsyncUDP API that DNSServer uses, under its own names: the frameworks have their own
// AsyncUDP.h / LwIPUdp.h. Packets are handled in lwIP's context, right in the receive callback. listen / close / sendTo
// from other tasks go through the tcpip thread with an OS, directly on the ESP8266 NONOS SDK (NO_SYS)

#include <Arduino.h>
// the ESP8266 core's Arduino.h has no IPAddress (the ESP32 and LibreTiny ones have)
#include <IPAddress.h>

#include <functional>

struct udp_pcb;

class EspurnaUdpMessage {
public:
  explicit EspurnaUdpMessage(size_t size = 1460);
  ~EspurnaUdpMessage();

  EspurnaUdpMessage(const EspurnaUdpMessage &) = delete;
  EspurnaUdpMessage &operator=(const EspurnaUdpMessage &) = delete;

  size_t write(const uint8_t *data, size_t len);
  size_t write(uint8_t data);

  size_t space() const;
  uint8_t *data();
  size_t length() const;

  explicit operator bool() const {
    return _buffer != nullptr;
  }

private:
  uint8_t *_buffer;
  size_t _index;
  size_t _size;
};

class EspurnaUdpPacket {
public:
  EspurnaUdpPacket(uint8_t *data, size_t len, const IPAddress &remoteIP, uint16_t remotePort)
    : _data(data), _len(len), _remoteIP(remoteIP), _remotePort(remotePort) {}

  uint8_t *data() {
    return _data;
  }

  size_t length() const {
    return _len;
  }

  IPAddress remoteIP() const {
    return _remoteIP;
  }

  uint16_t remotePort() const {
    return _remotePort;
  }

private:
  uint8_t *_data;
  size_t _len;
  IPAddress _remoteIP;
  uint16_t _remotePort;
};

class EspurnaUdp {
public:
  using PacketHandler = std::function<void(EspurnaUdpPacket &packet)>;

  EspurnaUdp() = default;
  ~EspurnaUdp();

  EspurnaUdp(const EspurnaUdp &) = delete;
  EspurnaUdp &operator=(const EspurnaUdp &) = delete;

  // set before listen(): the receive callback reads it in lwIP's context
  void onPacket(PacketHandler cb);

  bool listen(const IPAddress &addr, uint16_t port);
  bool listen(uint16_t port);
  void close();

  size_t sendTo(EspurnaUdpMessage &message, const IPAddress &addr, uint16_t port);

  bool connected() const {
    return _pcb != nullptr;
  }

private:
  friend struct EspurnaUdpLwip;

  udp_pcb *_pcb{nullptr};
  PacketHandler _handler;
};
