/*

CHIP DESCRIPTION AND LWIP GLUE, ESP32 part (ref. espurna_chip.h)

*/

#include "../../espurna.h"

#include "../../espurna_chip.h"


#include <chip_sdk.h>

#include <esp_idf_version.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <soc/rtc.h>

namespace espurna {
namespace chip {

WebInfo web_info() {
    return WebInfo{
        .arch = "esp32",
        .mcu = ::espurna::core::chip::chip_model(),
        .layout = nullptr,
        .tcm_used = 0,
        .tcm_size = 0,
    };
}

void info(::espurna::Output& out) {
    // the crystal is detected at boot (CONFIG_XTAL_FREQ_AUTO): boards come with 40 or 26 MHz
    out.printf_P(PSTR("mcu: %s rev%u chipid: %s freq: %hhumhz xtal: %umhz\n"),
            ::espurna::core::chip::chip_model(), ::espurna::core::chip::chip_revision(),
            systemChipId().c_str(), system_get_cpu_freq(),
            static_cast<unsigned>(rtc_clk_xtal_freq_get()));
}

void memory(::espurna::Output&) {
}

espurna::Ip4 address(const ip_addr_t* addr) {
    return espurna::Ip4(addr->u_addr.ip4.addr);
}

// Flash is split into partitions (ref. board_build.partitions), unlike the esp8266 fixed layout
bool storage(::espurna::Output& out, const char* mode) {
    out.printf_P(PSTR("speed: %u\n"), ::espurna::core::chip::flash_speed());
    out.printf_P(PSTR("mode: %s\n"), mode);
    out.printf_P(PSTR("size: %u\n"), ::espurna::core::chip::flash_size());

    const auto* running = esp_ota_get_running_partition();

    auto it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    for (; it != nullptr; it = esp_partition_next(it)) {
        const auto* part = esp_partition_get(it);
        out.printf_P(PSTR("%-8s [%08X...%08X) (%u bytes)%s\n"),
            part->label, part->address, part->address + part->size, part->size,
            (part == running) ? PSTR(" running") : PSTR(""));
    }
    esp_partition_iterator_release(it);

    out.printf_P(PSTR("app: %u bytes, free for OTA: %u bytes\n"),
        ::espurna::core::chip::sketch_size(), ::espurna::core::chip::sketch_space());

    return true;
}

espurna::string image() {
    return ESPURNA_IMAGE;
}


WebGuard web_guard() {
    return WebGuard{
        .heap = 16384,
        .block = 4096,
        .lwip = 0,
        .conns = 8,
        .requests = 0,
    };
}

// the SDK: ESP-IDF, no Arduino core (lib/espurna-core is the layer of the Arduino names)
espurna::string core_version() {
    return espurna::string("ESP-IDF ") + esp_get_idf_version();
}

// packed version (MAJOR << 16 | MINOR << 8 | PATCH)
espurna::string core_revision() {
    espurna::string out;
    out.reserve(16);
    out += espurna::string(ESP_IDF_VERSION_MAJOR);
    out += '.';
    out += espurna::string(ESP_IDF_VERSION_MINOR);
    out += '.';
    out += espurna::string(ESP_IDF_VERSION_PATCH);
    return out;
}

} // namespace chip
} // namespace espurna

// utils.h
espurna::string ipToString(const espurna::Ip4& ip) {
    espurna::string out;
    out.reserve(16);
    for (int index = 0; index < 4; ++index) {
        if (index) {
            out += '.';
        }
        out += espurna::string(ip[index]);
    }

    return out;
}
