/*

The network stack of the head (libs/net_impl.h: TCP, the stream, the HTTP server): the one implementation, every
chip. The modules call the head's; the HTTP client is the http module (http.cpp). The TLS session of a connection
(HTTP and MQTT) is the tls module's (module_tls.cpp, mbedtls), through the hook below

*/

#include "espurna.h"

#include "espurna_tasks.h"
#include "modules.h"

#define HTTP_CLIENT_AGENT "ESPurna"
#define NET_LOG(fmt, ...) DEBUG_MSG_P(PSTR("[NET] " fmt "\n"), ##__VA_ARGS__)
#define NET_LOCK ::espurna::system::Mutex
#include "libs/net_impl.h"

namespace espurna {
namespace net {
namespace lwip_tcp {

// TCP is the same on every chip (regress-todo 33, owner 2026-11-19 "full unification"), set where each SDK takes it:
// ESP8266 the SDK release (ESP8266_RTOS_SDK tools/espurna/sdkconfig.defaults), ESP32 code/sdkconfig.defaults, Beken the SDK release (platform-beken sdk/glue/lwipopts/)
// lwipopts.h. The numbers by the physics of the LAN: a segment fills the 1500 bytes of an Ethernet / Wi-Fi frame
// with the largest IP header (IPv6 40) and TCP 20; the receive window of 4 segments, the send buffer of 2 (the least
// lwIP allows); oversize a quarter segment - a small write (MQTT) takes 360 bytes more, not a whole segment (0 is not
// possible: the Wi-Fi drivers of the ESP8266 and the ESP32 send a frame from one pbuf, LWIP_NETIF_TX_SINGLE_PBUF, and
// lwip/init.c refuses it without oversize)
static_assert(TCP_MSS == 1500 - 40 - 20, "TCP_MSS of this chip is not 1440");
static_assert(TCP_WND == 4 * TCP_MSS, "TCP_WND of this chip is not 4 x TCP_MSS");
static_assert(TCP_SND_BUF == 2 * TCP_MSS, "TCP_SND_BUF of this chip is not 2 x TCP_MSS");
static_assert(TCP_OVERSIZE == TCP_MSS / 4, "TCP_OVERSIZE of this chip is not TCP_MSS / 4");

} // namespace lwip_tcp
} // namespace net

namespace tls {

// a TLS session is the tls module's (libs/tls_mbedtls.h). Without the module a secure connection fails, plain ones
// work (Apply + reboot installs it)
Session* open(tcp::Connection& tcp, const char* host, const Options& options, Failure& failure, int& error) {
#if MODULES_SUPPORT
    const auto func = modulesApi<decltype(open)>(modules::Api::TlsStart);
    if (func) {
        return func(tcp, host, options, failure, error);
    }
#endif
    failure = Failure::NoTls;
    error = 0;
    return nullptr;
}

} // namespace tls
} // namespace espurna
