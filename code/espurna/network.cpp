/*

NETWORKING MODULE

Copyright (C) 2022 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#include "espurna.h"

#include <forward_list>
#include <functional>
#include <memory>
#include <utility>

#include <lwip/init.h>
#include <lwip/dns.h>

#include "libs/URL.h"

#include "espurna_chip.h"

namespace espurna {
namespace network {
namespace dns {
namespace {

struct PendingHost {
    HostPtr ptr;
    HostCallback callback;
};

namespace internal {

using Pending = std::forward_list<PendingHost>;
Pending pending;

} // namespace internal

void dns_found_callback_impl(const char*, const ip_addr_t* addr, void* arg) {
    auto* pending = reinterpret_cast<Host*>(arg);

    if (addr) {
        pending->addr = chip::address(addr);
        pending->err = ERR_OK;
    } else {
        pending->err = ERR_ABRT;
    }

    internal::pending.remove_if(
        [&](const PendingHost& lhs) {
            if (lhs.ptr.get() == pending) {
                if (lhs.callback) {
                    lhs.callback(lhs.ptr);
                }

                return true;
            }

            return false;
        });
}

HostPtr resolve_impl(espurna::string hostname, HostCallback callback) {
    auto host = std::make_shared<Host>(
        Host{
            .name = std::move(hostname),
            .addr = espurna::Ip4{},
            .err = ERR_INPROGRESS,
        });

    const auto err = chip::gethostbyname(
        host->name.c_str(),
        host->addr,
        dns_found_callback_impl,
        host.get());

    host->err = err;

    switch (err) {
    case ERR_OK:
    case ERR_MEM:
        if (callback) {
            callback(host);
        }
        break;

    case ERR_INPROGRESS:
        internal::pending.push_front(
            PendingHost{
                .ptr = host,
                .callback = std::move(callback)
            });
        break;
    }

    return host;
}

} // namespace

HostPtr resolve(espurna::string hostname) {
    return resolve_impl(hostname, nullptr);
}

void resolve(espurna::string hostname, HostCallback callback) {
    if (!callback) {
        return;
    }

    resolve_impl(std::move(hostname), std::move(callback));
}

bool wait_for(HostPtr ptr, duration::Milliseconds timeout) {
    if (ptr->err == ERR_OK) {
        return true;
    }

    if (ptr->err != ERR_INPROGRESS) {
        return false;
    }

    time::blockingDelay(
        timeout,
        duration::Milliseconds{ 10 },
        [&]() {
            return ptr->err == ERR_INPROGRESS;
        });

    return ptr->err == ERR_OK;
}

espurna::Ip4 gethostbyname(espurna::string hostname, duration::Milliseconds timeout) {
    espurna::Ip4 out;

    auto result = resolve(hostname);
    if (wait_for(result, timeout)) {
        out = result->addr;
    }

    return out;
}

espurna::Ip4 gethostbyname(espurna::string hostname) {
    return gethostbyname(hostname, duration::Seconds{ 3 });
}

} // namespace dns

namespace {

#if TERMINAL_SUPPORT
namespace terminal {
namespace commands {

PROGMEM_STRING(Host, "HOST");

void host(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() != 2) {
        terminalError(ctx, "HOST <hostname>");
        return;
    }

    const auto result = dns::gethostbyname(ctx.argv[1]);
    if (chip::address_set(result)) {
        ctx.output.printf_P(PSTR("%s has address %s\n"),
            ctx.argv[1].c_str(), ipToString(result).c_str());
        terminalOK(ctx);
        return;
    }

    ctx.output.printf_P(PSTR("%s not found\n"), ctx.argv[1].c_str());
    terminalError(ctx, "Host not found");
}

PROGMEM_STRING(Netstat, "NETSTAT");

void netstat(::terminal::CommandContext&& ctx) {
    // lwIP pcb lists may belong to the tcpip task, can't be walked from loop() safely then
    if (!chip::netstat(ctx.output)) {
        terminalError(ctx, "NETSTAT is not implemented for ESP32");
    }
}

static constexpr ::terminal::Command List[] PROGMEM {
    {Host, host},
    {Netstat, netstat},
};

} // namespace commands

void setup() {
    espurna::terminal::add(commands::List);
}

} // namespace terminal
#endif

void setup() {
#if TERMINAL_SUPPORT
    terminal::setup();
#endif
}

} // namespace
} // namespace network
} // namespace espurna

void networkSetup() {
    espurna::network::setup();
}

