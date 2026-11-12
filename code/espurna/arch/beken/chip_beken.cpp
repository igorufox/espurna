/*

CHIP DESCRIPTION AND LWIP GLUE, LibreTiny part (ref. espurna_chip.h)

*/

#include "../../espurna.h"

#include "../../espurna_chip.h"

#if WEB_SUPPORT
#include "../../ws.h"
#endif

#include "layout_beken.h"

#include <lwip/stats.h>

namespace espurna {
namespace chip {

void ws_connected(JsonObject& root) {
    root[F("arch")] = F("beken");
    root[F("mcu")] = lt_cpu_get_model_name();

    // Beken bootloader (Tuya / Quectel), shown in the WebUI
    root[F("fwLayout")] = espurna::beken::layoutName(espurna::beken::flashLayout().layout);
}

void ws_visible(JsonObject& root) {
#if WEB_SUPPORT
    // static data in the TCM of the BK7231N, it does not change while running
    const auto tcm = systemTcmStats();
    if (tcm.size) {
        wsPayloadModule(root, STRING_VIEW("tcm"));
        root[F("tcmUsed")] = tcm.used;
        root[F("tcmSize")] = tcm.size;
    }
#endif
}

void info(Print& out) {
    out.printf_P(PSTR("mcu: %s chipid: %s freq: %hhumhz\n"),
            lt_cpu_get_model_name(),
            systemChipId().c_str(), system_get_cpu_freq());

    // bootloader / flash layout of the board, ref. arch/beken/layout_beken.cpp
    const auto& layout = ::espurna::beken::flashLayout();
    if (layout.installs_rbl) {
        out.printf_P(PSTR("flash: %u KiB, %s bootloader, OTA at 0x%06X\n"),
            static_cast<unsigned>(lt_flash_get_size() / 1024),
            ::espurna::beken::layoutName(layout.layout), layout.download_offset);
    } else {
        out.printf_P(PSTR("flash: %u KiB, %s bootloader 3.00, no firmware OTA (UART only)\n"),
            static_cast<unsigned>(lt_flash_get_size() / 1024),
            ::espurna::beken::layoutName(layout.layout));
    }
}

void memory(Print& out) {
#if LWIP_STATS && MEM_STATS
    // lwip has its own heap there, queued tcp data and received frames live in it
    out.printf_P(PSTR("lwip: size: %u used: %u max: %u errors: %u\n"),
            lwip_stats.mem.avail, lwip_stats.mem.used, lwip_stats.mem.max, lwip_stats.mem.err);
#endif

    const auto tcm = systemTcmStats();
    if (tcm.size) {
        out.printf_P(PSTR("tcm: used: %u free: %u size: %u\n"),
            tcm.used, tcm.size - tcm.used, tcm.size);
    }
}

IPAddress address(const ip_addr_t* addr) {
    return IPAddress(ip_addr_get_ip4_u32(addr));
}

// fixed layout, printed by the terminal itself
bool storage(Print&, const char*) {
    return false;
}

// the SDK does not track the largest free block; lwIP has its own heap (MEM_LIBC_MALLOC 0, 16 KB for queued TCP data
// and received frames). With Matter: 27.6 KB free, 15 parallel `GET /` took all of it (10.0.5.139, 2026-11-09)
// LibreTiny has no SPI library (no SPI.h): the RFM69 radio cannot be built for the BK7231N. The library's select()
// takes DIV16 on ARM chips
uint32_t rfm69_spi_divider() {
    return 16;
}

WebGuard web_guard() {
    return WebGuard{
        .heap = 8192,
        .block = 0,
        .lwip = 4096,
        .conns = 4,
    };
}

// SDK version is the LibreTiny one (ref. system_get_sdk_version()), core is the Beken BDK under it
// one head, two initial images: espurna-beken-tuya.uf2 (Tuya bootloader), espurna-beken-quectel.uf2 (Quectel)
String image() {
    String out(F(ESPURNA_IMAGE));
    if (out.length()) {
        switch (espurna::beken::flashLayout().layout) {
        case espurna::beken::Layout::Tuya:
            out += F("-tuya");
            break;
        case espurna::beken::Layout::Quectel:
            out += F("-quectel");
            break;
        case espurna::beken::Layout::Unknown:
            break;
        }
    }

    return out;
}

String core_version() {
    String out;
#if defined(CFG_BDK_VERSION)
    out.reserve(16);
    out += F("BDK ");
    out += String(CFG_BDK_VERSION / 10000);
    out += '.';
    out += String((CFG_BDK_VERSION / 100) % 100);
    out += '.';
    out += String(CFG_BDK_VERSION % 100);
#else
    out = F("LibreTiny");
#endif
    return out;
}

String core_revision() {
    return String(F("(unspecified)"));
}

} // namespace chip
} // namespace espurna

