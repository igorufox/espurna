/*

WIFI MODULE, esp8266 part of the types (ref. espurna_wifi.h)

ESP8266WiFi, lwIP and the NONOS SDK types the common code uses

*/

#pragma once

#include <Arduino.h>

#include <lwip/init.h>
#if LWIP_VERSION_MAJOR == 1
#include <netif/etharp.h>
#elif LWIP_VERSION_MAJOR >= 2
#include <lwip/etharp.h>
#endif

// (HACK) allow us to use internal lwip struct.
// esp8266 re-defines enum values from tcp header... include them first
#define LWIP_INTERNAL
#include <ESP8266WiFi.h>
#undef LWIP_INTERNAL

extern "C" {
  #include <lwip/opt.h>
  #include <lwip/ip.h>
  #include <lwip/tcp.h>
  #include <lwip/inet.h> // ip_addr_t
  #include <lwip/err.h> // ERR_x
  #include <lwip/dns.h> // dns_getserver, dns_gethostbyname
  #include <lwip/ip_addr.h> // ip4/ip6 helpers
};

// ref: https://github.com/me-no-dev/ESPAsyncTCP/pull/115/files#diff-e2e636049095cc1ff920c1bfabf6dcacR8
// This is missing with Core 2.3.0 and is sometimes missing from the build flags. Assume HIGH_BANDWIDTH version.
#ifndef TCP_MSS
#define TCP_MSS (1460)
#endif

namespace espurna {
namespace wifi {
namespace driver {

static constexpr AUTH_MODE AuthOpen { AUTH_OPEN };

using ScanResult = bss_info;

} // namespace driver
} // namespace wifi
} // namespace espurna
