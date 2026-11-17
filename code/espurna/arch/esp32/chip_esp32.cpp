/*

CHIP DESCRIPTION AND LWIP GLUE, ESP32 part (ref. espurna_chip.h)

*/

#include "../../espurna.h"

#include "../../espurna_chip.h"


#include <esp_arduino_version.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

namespace espurna {
namespace chip {

void ws_connected(JsonObject& root) {
    root[F("arch")] = F("esp32");
    root[F("mcu")] = ESP->getChipModel();
}

void ws_visible(JsonObject&) {
}

void info(Print& out) {
    out.printf_P(PSTR("mcu: %s rev%u chipid: %s freq: %hhumhz\n"),
            ESP->getChipModel(), ESP->getChipRevision(),
            systemChipId().c_str(), system_get_cpu_freq());
}

void memory(Print&) {
}

IPAddress address(const ip_addr_t* addr) {
    return IPAddress(addr->u_addr.ip4.addr);
}

// Flash is split into partitions (ref. board_build.partitions), unlike the esp8266 fixed layout
bool storage(Print& out, const char* mode) {
    out.printf_P(PSTR("speed: %u\n"), ESP.getFlashChipSpeed());
    out.printf_P(PSTR("mode: %s\n"), mode);
    out.printf_P(PSTR("size: %u\n"), ESP.getFlashChipSize());

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
        ESP.getSketchSize(), ESP.getFreeSketchSpace());

    return true;
}

String image() {
    return F(ESPURNA_IMAGE);
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

String core_version() {
    return ESP_ARDUINO_VERSION_STR;
}

// packed version (MAJOR << 16 | MINOR << 8 | PATCH)
String core_revision() {
    String out;
    out.reserve(16);
    out += String((ESP_ARDUINO_VERSION >> 16) & 0xFF);
    out += '.';
    out += String((ESP_ARDUINO_VERSION >> 8) & 0xFF);
    out += '.';
    out += String(ESP_ARDUINO_VERSION & 0xFF);
    return out;
}

} // namespace chip
} // namespace espurna

// the core's IPAddress has toString(), utils.h
String ipToString(const IPAddress& ip) {
    return ip.toString();
}
