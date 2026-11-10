// Asynchronous UDP of ESPurna, ref. EspurnaUdp.h
#include "EspurnaUdp.h"

#include <lwip/opt.h>
#include <lwip/ip.h>
#include <lwip/ip_addr.h>
#include <lwip/pbuf.h>
#include <lwip/udp.h>

#if !NO_SYS
#include <lwip/tcpip.h>
#include <lwip/priv/tcpip_priv.h>
#if __has_include(<freertos/task.h>)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#include <FreeRTOS.h>
#include <task.h>
#endif
#endif

#include <cstdlib>
#include <cstring>

struct EspurnaUdpLwip {
  struct Call;
  using Fn = err_t (*)(Call &);

  struct Call {
#if !NO_SYS
    tcpip_api_call_data base;  // first member, tcpip_api_call passes a pointer to it
#endif
    Fn fn;
    EspurnaUdp *udp;
    ip_addr_t addr;
    bool any;
    uint16_t port;
    const uint8_t *data;
    size_t len;
  };

  static void to_ip(ip_addr_t &out, const IPAddress &in) {
    IP_ADDR4(&out, in[0], in[1], in[2], in[3]);
  }

#if NO_SYS
  // one context for the sketch and lwIP, they never run at the same time
  static err_t run(Fn fn, Call &call) {
    return fn(call);
  }
#else
  // the tcpip thread, known from the first received packet. sendTo() from the receive callback (DNSServer replies
  // there) runs right away, tcpip_api_call would wait for the tcpip thread itself
  static TaskHandle_t tcpip_task;

  static err_t trampoline(tcpip_api_call_data *data) {
    auto *call = reinterpret_cast<Call *>(data);
    return call->fn(*call);
  }

  static err_t run(Fn fn, Call &call) {
    if (tcpip_task && (xTaskGetCurrentTaskHandle() == tcpip_task)) {
      return fn(call);
    }

    call.fn = fn;
    return tcpip_api_call(trampoline, &call.base);
  }
#endif

  static void recv_cb(void *arg, udp_pcb *, pbuf *p, const ip_addr_t *addr, u16_t port) {
#if !NO_SYS
    tcpip_task = xTaskGetCurrentTaskHandle();
#endif

    auto *udp = static_cast<EspurnaUdp *>(arg);
    if (p && addr && IP_IS_V4(addr) && udp->_handler) {
      // the first pbuf only, as the arduino-esp32 AsyncUDP. DNS queries fit in one
      EspurnaUdpPacket packet(static_cast<uint8_t *>(p->payload), p->len,
        IPAddress(static_cast<uint32_t>(ip_2_ip4(addr)->addr)), port);
      udp->_handler(packet);
    }

    if (p) {
      pbuf_free(p);
    }
  }

  static err_t listen_call(Call &call) {
    udp_pcb *pcb = udp_new();
    if (!pcb) {
      return ERR_MEM;
    }

#if SO_REUSE
    // the Beken SDK DHCP server holds port 53 with SO_REUSEADDR, udp_bind() fails without it
    ip_set_option(pcb, SOF_REUSEADDR);
#endif

    err_t err = udp_bind(pcb, call.any ? IP_ADDR_ANY : &call.addr, call.port);
    if (err != ERR_OK) {
      udp_remove(pcb);
      return err;
    }

    udp_recv(pcb, recv_cb, call.udp);
    call.udp->_pcb = pcb;

    return ERR_OK;
  }

  static err_t close_call(Call &call) {
    if (call.udp->_pcb) {
      udp_remove(call.udp->_pcb);
      call.udp->_pcb = nullptr;
    }

    return ERR_OK;
  }

  static err_t send_call(Call &call) {
    if (!call.udp->_pcb) {
      return ERR_CONN;
    }

    pbuf *p = pbuf_alloc(PBUF_TRANSPORT, call.len, PBUF_RAM);
    if (!p) {
      return ERR_MEM;
    }

    std::memcpy(p->payload, call.data, call.len);
    err_t err = udp_sendto(call.udp->_pcb, p, &call.addr, call.port);
    pbuf_free(p);

    return err;
  }
};

#if !NO_SYS
TaskHandle_t EspurnaUdpLwip::tcpip_task = nullptr;
#endif

EspurnaUdpMessage::EspurnaUdpMessage(size_t size)
  : _buffer(static_cast<uint8_t *>(std::malloc(size))), _index(0), _size(_buffer ? size : 0) {}

EspurnaUdpMessage::~EspurnaUdpMessage() {
  std::free(_buffer);
}

size_t EspurnaUdpMessage::write(const uint8_t *data, size_t len) {
  if (!_buffer) {
    return 0;
  }

  const size_t left = space();
  if (len > left) {
    len = left;
  }

  std::memcpy(_buffer + _index, data, len);
  _index += len;

  return len;
}

size_t EspurnaUdpMessage::write(uint8_t data) {
  return write(&data, 1);
}

size_t EspurnaUdpMessage::space() const {
  return _size - _index;
}

uint8_t *EspurnaUdpMessage::data() {
  return _buffer;
}

size_t EspurnaUdpMessage::length() const {
  return _index;
}

EspurnaUdp::~EspurnaUdp() {
  close();
}

void EspurnaUdp::onPacket(PacketHandler cb) {
  _handler = std::move(cb);
}

bool EspurnaUdp::listen(const IPAddress &addr, uint16_t port) {
  close();

  EspurnaUdpLwip::Call call{};
  call.udp = this;
  EspurnaUdpLwip::to_ip(call.addr, addr);
  call.any = false;
  call.port = port;

  return EspurnaUdpLwip::run(EspurnaUdpLwip::listen_call, call) == ERR_OK;
}

bool EspurnaUdp::listen(uint16_t port) {
  close();

  EspurnaUdpLwip::Call call{};
  call.udp = this;
  call.any = true;
  call.port = port;

  return EspurnaUdpLwip::run(EspurnaUdpLwip::listen_call, call) == ERR_OK;
}

void EspurnaUdp::close() {
  if (!_pcb) {
    return;
  }

  EspurnaUdpLwip::Call call{};
  call.udp = this;

  EspurnaUdpLwip::run(EspurnaUdpLwip::close_call, call);
}

size_t EspurnaUdp::sendTo(EspurnaUdpMessage &message, const IPAddress &addr, uint16_t port) {
  const size_t len = message.length();
  if (!_pcb || !len) {
    return 0;
  }

  EspurnaUdpLwip::Call call{};
  call.udp = this;
  EspurnaUdpLwip::to_ip(call.addr, addr);
  call.port = port;
  call.data = message.data();
  call.len = len;

  return (EspurnaUdpLwip::run(EspurnaUdpLwip::send_call, call) == ERR_OK) ? len : 0;
}
