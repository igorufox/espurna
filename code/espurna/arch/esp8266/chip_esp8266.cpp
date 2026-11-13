/*

CHIP DESCRIPTION AND LWIP GLUE, esp8266 part (ref. espurna_chip.h)

*/

#include "../../espurna.h"

#include "../../espurna_chip.h"


#include <lwip/init.h>
#include <lwip/tcp.h>

// not yet CONNECTING or LISTENING
extern "C" struct tcp_pcb *tcp_bound_pcbs;
// accepting or sending data
extern "C" struct tcp_pcb *tcp_active_pcbs;
// // TIME-WAIT status
extern "C" struct tcp_pcb *tcp_tw_pcbs;

namespace espurna {
namespace chip {

void ws_connected(JsonObject& root) {
    root[F("arch")] = F("esp8266");
    root[F("mcu")] = F("ESP8266");
}

void ws_visible(JsonObject&) {
}

void info(Print& out) {
    out.printf_P(PSTR("mcu: esp8266 chipid: %s freq: %hhumhz\n"),
            systemChipId().c_str(), system_get_cpu_freq());
}

void memory(Print&) {
}

bool netstat(Print& out) {
    const struct tcp_pcb* pcbs[] {
        tcp_active_pcbs,
        tcp_tw_pcbs,
        tcp_bound_pcbs,
    };

    for (const auto* list : pcbs) {
        for (const tcp_pcb* pcb = list; pcb != nullptr; pcb = pcb->next) {
            out.printf_P(PSTR("state %s local %s:%hu remote %s:%hu\n"),
                    tcp_debug_state_str(pcb->state),
                    IPAddress(pcb->local_ip).toString().c_str(),
                    pcb->local_port,
                    IPAddress(pcb->remote_ip).toString().c_str(),
                    pcb->remote_port);
        }
    }

    return true;
}

void wdt_pause() {
    ESP.wdtDisable();
}

void wdt_resume() {
    ESP.wdtEnable(5000);
}

IPAddress address(const ip_addr_t* addr) {
    return IPAddress(addr);
}

bool address_set(const IPAddress& ip) {
    return ip.isSet();
}

err_t gethostbyname(const char* name, IPAddress& out, dns_found_callback callback, void* arg) {
    return dns_gethostbyname(name, out, callback, arg);
}

// off, as upstream: no limit of connections or heap for new requests (owner, 2026-11-10). The settings turn it on

WebGuard web_guard() {
    return WebGuard{
        .heap = 0,
        .block = 0,
        .lwip = 0,
        .conns = 0,
    };
}

// fixed layout, printed by the terminal itself
bool storage(Print&, const char*) {
    return false;
}

String image() {
    return F(ESPURNA_IMAGE);
}

String core_version() {
    String out;
#ifdef ARDUINO_ESP8266_RELEASE
    out = ESP.getCoreVersion();
    if (out.equals("00000000")) {
        out = String(ARDUINO_ESP8266_RELEASE);
    }
    out.replace('_', '.');
#elif defined(ARDUINO_ESP8266_GIT_DESC)
#define _GET_COREVERSION_STR(X) #X
#define GET_COREVERSION_STR(X) _GET_COREVERSION_STR(X)
    out = GET_COREVERSION_STR(ARDUINO_ESP8266_GIT_DESC);
#undef _GET_COREVERSION_STR
#undef GET_COREVERSION_STR
#else
    out = "unknown";
#endif
    return out;
}

String core_revision() {
#ifdef ARDUINO_ESP8266_GIT_VER
    return String(ARDUINO_ESP8266_GIT_VER, 16);
#else
    return String(F("(unspecified)"));
#endif
}

} // namespace chip
} // namespace espurna

bool equalsConstantTime(const String& lhs, const String& rhs) {
    return lhs.equalsConstantTime(rhs);
}

// the core's IPAddress has toString(), utils.h
String ipToString(const IPAddress& ip) {
    return ip.toString();
}
