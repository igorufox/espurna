/*

NET - the implementations of the one network stack of everything ESPurna builds, included by exactly ONE .cpp of an
image: ESPurna's net.cpp (the head of every chip), code/migration-app's and code/safeboot-app's main.cpp.
- libs/tcp.h            the TCP connection and listener on lwIP's raw API
- libs/tls.h            the byte stream over TCP, plain or TLS (BearSSL); under the HTTP and the MQTT client
- libs/http_client.h    the HTTP client
- libs/http_server.h    the HTTP server
(the WebSocket server, libs/websocket.h, is not part of it: the ws module of the head has it, ws_server.cpp)

What that .cpp defines before the include is only how the image logs and locks, never what the stack does:
    NET_LOCK            the lock between lwIP's callbacks and the loop (lock(), unlock()); a no-op one without an OS
    NET_LOG(fmt, ...)   one log line (fmt a string literal)
    HTTP_CLIENT_AGENT   the User-Agent of the client

*/

#pragma once

// The lwIP version of code/libraries.ini (passed by every config as -DESPURNA_LWIP): lwIP comes built into the
// platform of the chip, so the build stops here when a platform brought another one. Two versions on purpose
// (owner 2026-11-12): 2.2.0 on ESP32 / Beken / safeboot (their SDKs), 2.1.3 on ESP8266 / migration image (the NONOS core has it
// built in, no 2.2 port of its link layer exists; ESPurna uses only the raw TCP API, DNS and tcpip_api_call(), the
// same in both). The reasons in full: code/libraries.ini, `lwip`
#include <lwip/init.h>

#ifndef ESPURNA_LWIP
#error "ESPURNA_LWIP is not set: the config must pass -DESPURNA_LWIP=\"${libraries.lwip}\" (code/libraries.ini)"
#endif

namespace espurna {
namespace net {
namespace lwip_version {

// MAJOR.MINOR.REVISION of the platform's lwIP (LWIP_VERSION_STRING) is the one of libraries.ini; only a suffix may
// follow it: ESP-IDF builds esp-lwip, Espressif's fork of the lwIP 2.2.0 development branch ("2.2.0d"), into its
// precompiled libraries - the one thing of lwIP the ESP32 SDK does not let us choose
constexpr bool same(const char* lhs, const char* rhs) {
    return (*rhs == '\0')
        ? ((*lhs == '\0') || ((*lhs >= 'a') && (*lhs <= 'z')))
        : ((*lhs == *rhs) && same(lhs + 1, rhs + 1));
}

static_assert(same(LWIP_VERSION_STRING, ESPURNA_LWIP),
    "the lwIP of this platform is not the version of code/libraries.ini (lwip / lwip_esp8266)");

} // namespace lwip_version
} // namespace net
} // namespace espurna

#include "tcp_impl.h"
#include "tls_impl.h"
#include "http_client_impl.h"
#include "http_server_impl.h"
