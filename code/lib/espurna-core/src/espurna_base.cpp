// The calls of espurna-base (<espurna_base.h>) that are not Arduino names: the libraries of the modules (FauxmoESP,
// IRremoteESP8266) take them as they took the Arduino core's. millis(), pinMode(), attachInterrupt() ... of the same
// header are wiring.cpp's (the C symbols of ArduinoCore-API)

#include "core_chip.h"

#include <mbedtls/md5.h>

#include <stddef.h>
#include <stdint.h>

namespace chip = ::espurna::core::chip;

extern "C" {

void espurna_restart(void) {
    chip::restart();
}

void espurna_local_ip(uint8_t out[4]) {
    uint32_t ip { 0 };
    uint32_t netmask { 0 };
    uint32_t gateway { 0 };
    if (!chip::wifi_sta_address(ip, netmask, gateway)) {
        ip = 0;
    }

    // network order
    out[0] = ip & 0xff;
    out[1] = (ip >> 8) & 0xff;
    out[2] = (ip >> 16) & 0xff;
    out[3] = (ip >> 24) & 0xff;
}

void espurna_mac(uint8_t out[6]) {
    chip::wifi_mac(false, out);
}

void espurna_md5(const void* data, size_t size, uint8_t out[16]) {
    mbedtls_md5_context context;
    mbedtls_md5_init(&context);
    mbedtls_md5_starts(&context);
    mbedtls_md5_update(&context, static_cast<const unsigned char*>(data), size);
    mbedtls_md5_finish(&context, out);
    mbedtls_md5_free(&context);
}

} // extern "C"
