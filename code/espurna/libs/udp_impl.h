/*

UDP - the implementation of libs/udp.h, part of libs/net_impl.h

*/

#pragma once

#include "udp.h"
#include "lwip_call.h"

#include <lwip/opt.h>
#include <lwip/dns.h>
#include <lwip/igmp.h>
#include <lwip/ip.h>
#include <lwip/ip_addr.h>
#include <lwip/pbuf.h>
#include <lwip/udp.h>

namespace espurna {
namespace udp {

class Socket::Impl {
public:
    // in lwIP's context: right away; elsewhere through the tcpip thread (lwipCall), waited for. A reply sent from the
    // receive callback (DNSServer, LLMNR, NetBIOS, SSDP) runs in the tcpip thread itself, a message to it would wait
    // for it for ever
    template <typename F>
    static void run(F&& func) {
#if NO_SYS
        func();
#else
        if (tcpip_task && (xTaskGetCurrentTaskHandle() == tcpip_task)) {
            func();
            return;
        }

        lwip::lwipCall(func);
#endif
    }

    static void receive(void* arg, udp_pcb*, pbuf* p, const ip_addr_t* addr, u16_t port) {
#if !NO_SYS
        tcpip_task = xTaskGetCurrentTaskHandle();
#endif

        auto* socket = static_cast<Socket*>(arg);
        if (p && addr && IP_IS_V4(addr) && socket->_handler) {
            // the first pbuf only, as the arduino-esp32 AsyncUDP did. DNS queries, SSDP and espota invitations fit
            Packet packet(static_cast<uint8_t*>(p->payload), p->len, ip_2_ip4(addr)->addr, port);
            socket->_handler(packet);
        }

        if (p) {
            pbuf_free(p);
        }
    }

    static bool listen(Socket& socket, const ip_addr_t* address, uint16_t port) {
        err_t err { ERR_MEM };
        run([&]() {
            udp_pcb* pcb = udp_new();
            if (!pcb) {
                return;
            }

#if SO_REUSE
            // the Beken SDK's DHCP server holds port 53 with SO_REUSEADDR, udp_bind() fails without it
            ip_set_option(pcb, SOF_REUSEADDR);
#endif

            err = udp_bind(pcb, address, port);
            if (err != ERR_OK) {
                udp_remove(pcb);
                return;
            }

            udp_recv(pcb, receive, &socket);
            socket._pcb = pcb;
        });

        return err == ERR_OK;
    }

    static bool group(uint32_t group, bool join) {
#if LWIP_IGMP
        err_t err { ERR_VAL };
        run([&]() {
            ip4_addr_t address;
            address.addr = group;
            err = join
                ? igmp_joingroup(IP4_ADDR_ANY4, &address)
                : igmp_leavegroup(IP4_ADDR_ANY4, &address);
        });

        return err == ERR_OK;
#else
        (void)group;
        (void)join;
        return false;
#endif
    }

#if LWIP_DNS
    // a name being resolved: on the heap until both lwIP's answer and the caller are done with it (the caller gives
    // up after its timeout, the answer may come later)
    struct Resolve {
        ip_addr_t address;
        volatile bool done;
        volatile bool found;
        bool abandoned;
    };

    static void found(const char*, const ip_addr_t* address, void* arg) {
        auto* resolve = static_cast<Resolve*>(arg);
        if (resolve->abandoned) {
            std::free(resolve);
            return;
        }

        if (address && IP_IS_V4(address)) {
            ip_addr_copy(resolve->address, *address);
            resolve->found = true;
        }
        resolve->done = true;
    }
#endif

#if !NO_SYS
    // the tcpip thread, known from the first received packet
    static TaskHandle_t tcpip_task;
#endif
};

#if !NO_SYS
TaskHandle_t Socket::Impl::tcpip_task { nullptr };
#endif

Socket::~Socket() {
    close();
}

bool Socket::listen(uint16_t port) {
    close();
    return Impl::listen(*this, IP_ADDR_ANY, port);
}

bool Socket::listen(uint32_t ip, uint16_t port) {
    close();

    ip_addr_t address;
    IP_ADDR4(&address, ip & 0xff, (ip >> 8) & 0xff, (ip >> 16) & 0xff, (ip >> 24) & 0xff);
    return Impl::listen(*this, &address, port);
}

void Socket::close() {
    if (!_pcb) {
        return;
    }

    Impl::run([&]() {
        udp_remove(_pcb);
        _pcb = nullptr;
    });
}

bool Socket::joinGroup(uint32_t group) {
    return Impl::group(group, true);
}

bool Socket::leaveGroup(uint32_t group) {
    return Impl::group(group, false);
}

size_t Socket::sendTo(const uint8_t* data, size_t len, uint32_t ip, uint16_t port) {
    if (!_pcb || !data || !len) {
        return 0;
    }

    err_t err { ERR_MEM };
    Impl::run([&]() {
        if (!_pcb) {
            err = ERR_CONN;
            return;
        }

        pbuf* p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
        if (!p) {
            return;
        }

        std::memcpy(p->payload, data, len);

        ip_addr_t address;
        IP_ADDR4(&address, ip & 0xff, (ip >> 8) & 0xff, (ip >> 16) & 0xff, (ip >> 24) & 0xff);
        err = udp_sendto(_pcb, p, &address, port);
        pbuf_free(p);
    });

    return (err == ERR_OK) ? len : 0;
}

bool Socket::resolve(const char* host, uint32_t& out, uint32_t timeout) {
    if (!host || !*host) {
        return false;
    }

    ip4_addr_t numeric;
    if (ip4addr_aton(host, &numeric)) {
        out = numeric.addr;
        return true;
    }

#if LWIP_DNS && !NO_SYS
    auto* resolve = static_cast<Impl::Resolve*>(std::calloc(1, sizeof(Impl::Resolve)));
    if (!resolve) {
        return false;
    }

    Impl::run([&]() {
        const err_t err = dns_gethostbyname(host, &resolve->address, Impl::found, resolve);
        if (err == ERR_OK) {
            resolve->found = IP_IS_V4(&resolve->address);
            resolve->done = true;
        } else if (err != ERR_INPROGRESS) {
            resolve->done = true;
        }
    });

    constexpr uint32_t Step { 10 };
    for (uint32_t waited = 0; !resolve->done && (waited < timeout); waited += Step) {
        vTaskDelay(pdMS_TO_TICKS(Step));
    }

    const bool found = resolve->done && resolve->found;
    if (found) {
        out = ip_2_ip4(&resolve->address)->addr;
    }

    // in lwIP's context, so the answer and this never cross
    Impl::run([&]() {
        if (resolve->done) {
            std::free(resolve);
        } else {
            resolve->abandoned = true;
        }
    });

    return found;
#else
    (void)timeout;
    return false;
#endif
}

} // namespace udp
} // namespace espurna
