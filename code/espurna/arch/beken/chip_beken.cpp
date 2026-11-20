/*

CHIP DESCRIPTION AND LWIP GLUE, BK72xx part (ref. espurna_chip.h)

*/

#include "beken_api.h"
#include "../../espurna.h"

#include "../../espurna_chip.h"

#include "layout_beken.h"

#include <lwip/stats.h>

namespace espurna {
namespace chip {

// the Beken bootloader (Tuya / Quectel) and the static data in the TCM of the BK7231N (it does not change while
// running), shown in the WebUI
WebInfo web_info() {
    const auto tcm = systemTcmStats();
    return WebInfo{
        .arch = "beken",
        .mcu = ::espurna::beken::cpu_model(),
        .layout = espurna::beken::layoutName(espurna::beken::flashLayout().layout),
        .tcm_used = static_cast<uint32_t>(tcm.used),
        .tcm_size = static_cast<uint32_t>(tcm.size),
    };
}

void info(::espurna::Output& out) {
    out.printf_P(PSTR("mcu: %s chipid: %s freq: %hhumhz\n"),
            ::espurna::beken::cpu_model(),
            systemChipId().c_str(), system_get_cpu_freq());

    // bootloader / flash layout of the board, ref. arch/beken/layout_beken.cpp
    const auto& layout = ::espurna::beken::flashLayout();
    if (layout.installs_rbl) {
        out.printf_P(PSTR("flash: %u KiB, %s bootloader, OTA at 0x%06X\n"),
            static_cast<unsigned>(::espurna::beken::flash_size() / 1024),
            ::espurna::beken::layoutName(layout.layout), layout.download_offset);
    } else {
        out.printf_P(PSTR("flash: %u KiB, %s bootloader 3.00, no firmware OTA (UART only)\n"),
            static_cast<unsigned>(::espurna::beken::flash_size() / 1024),
            ::espurna::beken::layoutName(layout.layout));
    }
}

void memory(::espurna::Output& out) {
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

espurna::Ip4 address(const ip_addr_t* addr) {
    return espurna::Ip4(ip_addr_get_ip4_u32(addr));
}

// fixed layout, printed by the terminal itself
bool storage(::espurna::Output&, const char*) {
    return false;
}

// the SDK does not track the largest free block; lwIP has its own heap (MEM_LIBC_MALLOC 0, 16 KB for queued TCP data
// and received frames). With Matter: 27.6 KB free, 15 parallel `GET /` took all of it (10.0.5.139, 2026-11-09)

WebGuard web_guard() {
    return WebGuard{
        .heap = 8192,
        .block = 0,
        .lwip = 4096,
        .conns = 4,
        .requests = 0,
    };
}

// the core is the Beken BDK (CFG_BDK_VERSION of scripts/beken_bdk.py)
// one head, two initial images: espurna-beken-tuya.uf2 (Tuya bootloader), espurna-beken-quectel.uf2 (Quectel)
espurna::string image() {
    espurna::string out(ESPURNA_IMAGE);
    if (out.length()) {
        switch (espurna::beken::flashLayout().layout) {
        case espurna::beken::Layout::Tuya:
            out += "-tuya";
            break;
        case espurna::beken::Layout::Quectel:
            out += "-quectel";
            break;
        case espurna::beken::Layout::Unknown:
            break;
        }
    }

    return out;
}

espurna::string core_version() {
    espurna::string out;
    out.reserve(16);
    out += "BDK ";
    out += espurna::string(CFG_BDK_VERSION / 10000);
    out += '.';
    out += espurna::string((CFG_BDK_VERSION / 100) % 100);
    out += '.';
    out += espurna::string(CFG_BDK_VERSION % 100);
    return out;
}

espurna::string core_revision() {
    return espurna::string("(unspecified)");
}

} // namespace chip
} // namespace espurna

// utils.h
espurna::string ipToString(const espurna::Ip4& ip) {
    char out[16];
    ip.toString(out, sizeof(out));
    return espurna::string(out);
}
