/*

CHIP DESCRIPTION AND LWIP GLUE, FreeRTOS chips part: ESP32 and LibreTiny (ref. espurna_chip.h)

Dual stack lwIP of both SDKs and the ArduinoCore-API style String / Stream. What calls the SDK of a chip
is in arch/<chip>/chip_<chip>.cpp

*/

#include "../../espurna.h"

#if defined(ESP32) || defined(LIBRETINY)

#include "../../espurna_chip.h"

namespace espurna {
namespace chip {

void wdt_pause() {
}

void wdt_resume() {
    system::chip::sdk::feed_loop_wdt();
}

// lwIP pcb lists belong to the tcpip task here
bool netstat(Print&) {
    return false;
}

bool address_set(const IPAddress& ip) {
    return (uint32_t)ip != 0;
}

err_t gethostbyname(const char* name, IPAddress& out, dns_found_callback callback, void* arg) {
    ip_addr_t addr;
    const auto err = dns_gethostbyname(name, &addr, callback, arg);
    if (err == ERR_OK) {
        out = address(&addr);
    }

    return err;
}

} // namespace chip
} // namespace espurna

// Not every core String has equalsConstantTime() (ArduinoCore-API of LibreTiny does not). Every byte is compared,
// time only depends on the length
bool equalsConstantTime(const String& lhs, const String& rhs) {
    if (lhs.length() != rhs.length()) {
        return false;
    }

    uint8_t diff = 0;
    for (size_t index = 0; index < lhs.length(); ++index) {
        diff |= static_cast<uint8_t>(lhs[index]) ^ static_cast<uint8_t>(rhs[index]);
    }

    return diff == 0;
}

#endif
