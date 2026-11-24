/*

CHIP DESCRIPTION AND LWIP GLUE, BK72xx part (ref. espurna_chip.h)

*/

#include "beken_api.h"
#include "../../espurna.h"

#include "../../espurna_chip.h"

#include "layout_beken.h"

#include <lwip/pbuf.h>
#include <lwip/stats.h>
#include <lwip/udp.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <FreeRTOS.h>
#include <queue.h>
#include <task.h>

// BDK rw_msdu.c: the netif a vif hands its frames to
extern "C" void* rwm_mgmt_get_vif2netif(uint8_t vif_idx);

extern "C" {

void __real_ethernetif_input(int iface, struct pbuf* p);
void* net_get_sta_handle(void);

} // extern "C"

// The EAPOL frames of the station, a little later. An AP that answers fast (tiger, 2.4 GHz HE20) sends the first
// message of the 4-way handshake before the supplicant has taken the association event: it delays the frame ("Not
// associated - Delay processing of received EAPOL frame (state=ASSOCIATING bssid=00:...)"), works the handshake out
// from it, then the association event resets that, and the pairwise key is never installed (no hapd_intf_add_key, no
// WLAN_EVENT_CONNECTED). The AP counts the handshake done, every unicast frame after it stays encrypted for the
// station: no DHCP, the attempt times out (214 2026-11-23, serial log). A slower AP (lion, duck) gives the supplicant the
// time. The frames go to the BDK in their order, EapolDelay later, from a task of their own: under the 100 ms after
// which hostapd repeats the first message (300 ms crossed that: two answers, PSK-MISMATCH on the AP); the AP waits a
// second for each answer and repeats
namespace {

constexpr TickType_t EapolDelay { pdMS_TO_TICKS(80) };

struct Eapol {
    int iface;
    struct pbuf* p;
    TickType_t due;
};

QueueHandle_t eapol_queue { nullptr };

void eapol_task(void*) {
    Eapol item;
    for (;;) {
        if (xQueueReceive(eapol_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const TickType_t now = xTaskGetTickCount();
        if (static_cast<int32_t>(item.due - now) > 0) {
            vTaskDelay(item.due - now);
        }

        __real_ethernetif_input(item.iface, item.p);
    }
}

bool eapol_station(int iface, const struct pbuf* p) {
    const auto* data = static_cast<const uint8_t*>(p->payload);
    return (p->len >= 14) && (data[12] == 0x88) && (data[13] == 0x8e)
        && (rwm_mgmt_get_vif2netif(static_cast<uint8_t>(iface)) == net_get_sta_handle());
}

bool eapol_ready { false };
bool eapol_failed { false };

bool eapol_later(int iface, struct pbuf* p) {
    if (!eapol_ready) {
        if (eapol_failed) {
            return false;
        }

        eapol_queue = xQueueCreate(8, sizeof(Eapol));
        if (!eapol_queue || (xTaskCreate(eapol_task, "eapol", 512, nullptr, 5, nullptr) != pdPASS)) {
            eapol_failed = true;
            return false;
        }

        eapol_ready = true;
    }

    const Eapol item{iface, p, xTaskGetTickCount() + EapolDelay};
    return xQueueSend(eapol_queue, &item, 0) == pdTRUE;
}

} // namespace

// DHCP answers broadcast (the BOOTP broadcast flag in every request of the lwIP client). dnsmasq sends its OFFER / ACK
// with DSCP CS6: an AP sends that to the station in WMM voice, TID 6/7, as a single frame. Behind tiger (MT7986,
// 2.4 GHz HE20, hardware A-MSDU) the Beken MAC drops every such frame - the last sequence number of TID 7 kept, its
// receive PN never moves (sta_info_tab, 214 2026-11-23) - so the station never got an address there; broadcast
// frames reach it from every AP. Linked with --wrap=udp_sendto_if, --wrap=udp_sendto_if_src (platformio_beken.ini;
// lwIP's dhcp.c calls both)
namespace {

constexpr uint16_t DhcpClientPort { 68 };
constexpr uint16_t DhcpServerPort { 67 };
constexpr uint8_t BootRequest { 1 };
constexpr size_t BootpFlags { 10 };

void dhcp_broadcast_flag(const struct udp_pcb* pcb, struct pbuf* p, uint16_t dst_port) {
    if (pcb && p && (pcb->local_port == DhcpClientPort) && (dst_port == DhcpServerPort)
        && (p->len > (BootpFlags + 1)))
    {
        auto* data = static_cast<uint8_t*>(p->payload);
        if (data[0] == BootRequest) {
            data[BootpFlags] |= 0x80;
        }
    }
}

} // namespace

extern "C" {

err_t __real_udp_sendto_if(struct udp_pcb* pcb, struct pbuf* p, const ip_addr_t* dst_ip, u16_t dst_port,
    struct netif* netif);
err_t __real_udp_sendto_if_src(struct udp_pcb* pcb, struct pbuf* p, const ip_addr_t* dst_ip, u16_t dst_port,
    struct netif* netif, const ip_addr_t* src_ip);

err_t __wrap_udp_sendto_if(struct udp_pcb* pcb, struct pbuf* p, const ip_addr_t* dst_ip, u16_t dst_port,
    struct netif* netif)
{
    dhcp_broadcast_flag(pcb, p, dst_port);
    return __real_udp_sendto_if(pcb, p, dst_ip, dst_port, netif);
}

err_t __wrap_udp_sendto_if_src(struct udp_pcb* pcb, struct pbuf* p, const ip_addr_t* dst_ip, u16_t dst_port,
    struct netif* netif, const ip_addr_t* src_ip)
{
    dhcp_broadcast_flag(pcb, p, dst_port);
    return __real_udp_sendto_if_src(pcb, p, dst_ip, dst_port, netif, src_ip);
}

void __wrap_ethernetif_input(int iface, struct pbuf* p) {
    if (p && (iface >= 0) && eapol_station(iface, p) && eapol_later(iface, p)) {
        return;
    }

    __real_ethernetif_input(iface, p);
}

} // extern "C"

// The BDK prints the secrets of the station to its UART: the closed supplicant writes the Wi-Fi passphrase on every
// start ("PSKC: ssid ..., passphrase ..."), the pairwise and group keys of every handshake ("WPA: TK ...", "WPA:
// GTK ..."; 214 2026-11-23, the serial log). Those lines are dropped, everything else goes out as before. Linked with
// --wrap=bk_printf (platformio_beken.ini)
extern "C" {

void __real_bk_printf(const char* fmt, ...);

void __wrap_bk_printf(const char* fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    if (std::strstr(line, "passphrase") || std::strstr(line, "WPA: TK") || std::strstr(line, "WPA: GTK")
        || std::strstr(line, "WPA: PMK") || std::strstr(line, "PSKC:"))
    {
        return;
    }

    __real_bk_printf("%s", line);
}

} // extern "C"

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
    out.printf_P(PSTR("mcu: %s chipid: %s freq: %umhz\n"),
            ::espurna::beken::cpu_model(),
            systemChipId().c_str(), static_cast<unsigned>(system_get_cpu_freq()));

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
