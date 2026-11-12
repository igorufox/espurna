/*

WEB GUARD

New TCP connections to the web server and telnet wait while the heap is low. The same code on every chip.

Every request of the web server holds several KB until its answer is sent (the request, the response buffer, the
queued TCP data). Parallel requests ran the heap out:
- BK7231N running Matter (27.6 KB free): 15 parallel `GET /` took it to 0, the loop stood for 8.5 s, watchdog restart
  (10.0.5.139, 2026-11-09)
- ESP8266 1 MB head with modules (4-9 KB free): two WebUI tabs, failed allocations and an OOM abort in the server's
  response (10.0.5.0, 2026-11-09). Clients that time out and ask again pile up requests until the heap is gone.

A gate sits between lwIP and the server: the accept callback of the listening pcb (lpcb->accept) of the web server
and of telnet. A new connection is reset (the browser tries again) instead of taken when
- its port already has webGuardConns connections (the new one and web sockets included),
- the free heap is below webGuardHeap,
- the largest free block is below webGuardBlock,
- lwIP's own heap (where lwIP has one, MEM_LIBC_MALLOC 0: the BK7231N) has less than webGuardLwip free.
0 turns a check off; defaults per chip: espurna::chip::web_guard() (espurna_chip.h). Connections already open are never
closed (the WebUI stays connected): the guard recovers by itself, the heap comes back as they finish.

No server or library code changes: the loop puts the gate on the listeners every few seconds (a server started later
or again gets it as well). lwIP's pcb lists are walked in lwIP's context: through the tcpip thread with an OS, directly
on the ESP8266 NONOS SDK (NO_SYS). The gate itself runs in lwIP's context, thresholds are copied there from the
settings by the loop.

Chip specific: the free heap and the largest block come from systemFreeHeap() / systemHeapStats() (arch/<chip>/
system_<chip>.cpp). The ESP8266 head keeps its SDK reserve in heap_reserve_esp8266.cpp.

*/

#include "espurna.h"

#if WEB_SUPPORT

#include "web.h"
#include "web_guard.h"
#include "espurna_chip.h"

#if TELNET_SUPPORT
#include "telnet.h"
#endif

#include <lwip/opt.h>
#include <lwip/tcp.h>
#include <lwip/priv/tcp_priv.h>
#include <lwip/stats.h>

#if !NO_SYS
#include <lwip/tcpip.h>
#include <lwip/priv/tcpip_priv.h>
#endif

namespace espurna {
namespace web {
namespace guard {
namespace {

namespace build {

// defaults are the chip's, ref. espurna_chip.h
uint32_t heap() {
    return espurna::chip::web_guard().heap;
}

uint32_t block() {
    return espurna::chip::web_guard().block;
}

uint32_t lwip() {
    return espurna::chip::web_guard().lwip;
}

uint32_t conns() {
    return espurna::chip::web_guard().conns;
}

// how often the loop looks for listeners without the gate
constexpr uint32_t ScanInterval { 5000 };

} // namespace build

namespace settings {
namespace keys {

STRING_VIEW_INLINE(Heap, "webGuardHeap");
STRING_VIEW_INLINE(Block, "webGuardBlock");
STRING_VIEW_INLINE(Lwip, "webGuardLwip");
STRING_VIEW_INLINE(Conns, "webGuardConns");

} // namespace keys

uint32_t heap() {
    return getSetting(keys::Heap, build::heap());
}

uint32_t block() {
    return getSetting(keys::Block, build::block());
}

uint32_t lwip() {
    return getSetting(keys::Lwip, build::lwip());
}

uint32_t conns() {
    return getSetting(keys::Conns, build::conns());
}

} // namespace settings

// the original accept callback of a listener, by its port (set once, the listeners are made once at boot)
struct Listener {
    void* arg;
    tcp_accept_fn accept;
    uint16_t port;
};

// allocated at setup: no static RAM on the ESP8266
struct State {
    Listener listeners[2];
    uint32_t heap;
    uint32_t block;
    uint32_t lwip;
    uint32_t conns;
    uint32_t refused;
    uint32_t reported;
    uint32_t scanned;
};

State* state { nullptr };

// --- lwIP context --------------------------------------------------------------------------------------------------

// connections to this local port (the new one is in the list already)
uint32_t connections(uint16_t port) {
    uint32_t out = 0;
    for (auto* pcb = tcp_active_pcbs; pcb; pcb = pcb->next) {
        if ((pcb->local_port == port) && (pcb->state != TIME_WAIT)) {
            ++out;
        }
    }

    return out;
}

// free bytes of lwIP's own heap; without one lwIP allocates from the heap checked above
uint32_t lwipFree() {
#if !MEM_LIBC_MALLOC && LWIP_STATS && MEM_STATS
    return (lwip_stats.mem.avail > lwip_stats.mem.used)
        ? static_cast<uint32_t>(lwip_stats.mem.avail - lwip_stats.mem.used)
        : 0;
#else
    return std::numeric_limits<uint32_t>::max();
#endif
}

bool admit(const State& guard, uint16_t port) {
    if (guard.conns && (connections(port) > guard.conns)) {
        return false;
    }

    if (guard.heap && (systemFreeHeap() < guard.heap)) {
        return false;
    }

    if (guard.block && (systemHeapStats().usable < guard.block)) {
        return false;
    }

    if (guard.lwip && (lwipFree() < guard.lwip)) {
        return false;
    }

    return true;
}

err_t gate(void* arg, struct tcp_pcb* pcb, err_t err) {
    auto& guard = *state;

    // lwIP passes no pcb when it could not make one (ERR_MEM)
    const Listener* listener = nullptr;
    for (const auto& entry : guard.listeners) {
        if (entry.accept && (pcb ? (entry.port == pcb->local_port) : (entry.arg == arg))) {
            listener = &entry;
            break;
        }
    }

    if (pcb && (ERR_OK == err) && (!listener || !admit(guard, pcb->local_port))) {
        ++guard.refused;
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    return listener ? listener->accept(arg, pcb, err) : ERR_VAL;
}

void install() {
    auto& guard = *state;
    for (auto* lpcb = tcp_listen_pcbs.listen_pcbs; lpcb; lpcb = lpcb->next) {
        if (!lpcb->accept || (lpcb->accept == gate)) {
            continue;
        }

        for (auto& listener : guard.listeners) {
            if (listener.port && (listener.port == lpcb->local_port)) {
                listener.arg = lpcb->callback_arg;
                listener.accept = lpcb->accept;
                lpcb->accept = gate;
                break;
            }
        }
    }
}

#if NO_SYS
void installInLwip() {
    install();
}
#else
err_t installCall(struct tcpip_api_call_data*) {
    install();
    return ERR_OK;
}

void installInLwip() {
    struct tcpip_api_call_data call {};
    tcpip_api_call(installCall, &call);
}
#endif

// --- loop ----------------------------------------------------------------------------------------------------------

void configure() {
    auto& guard = *state;
    guard.heap = settings::heap();
    guard.block = settings::block();
    guard.lwip = settings::lwip();
    guard.conns = settings::conns();
}

void loop() {
    auto& guard = *state;

    const auto now = millis();
    if (now - guard.scanned < build::ScanInterval) {
        return;
    }

    guard.scanned = now;
    installInLwip();

    const auto refused = guard.refused;
    if (refused != guard.reported) {
        DEBUG_MSG_P(PSTR("[WEBSERVER] Low heap guard: %u new connection(s) reset, free heap %u\n"),
            static_cast<unsigned>(refused - guard.reported),
            static_cast<unsigned>(systemFreeHeap()));
        guard.reported = refused;
    }
}

void setup() {
    state = new State{};

    state->listeners[0].port = webPort();
#if TELNET_SUPPORT
    state->listeners[1].port = telnetPort();
#endif

    configure();
    installInLwip();
    state->scanned = millis();

    ::espurnaRegisterReload(configure);
    ::espurnaRegisterLoop(loop);
}

} // namespace
} // namespace guard
} // namespace web
} // namespace espurna

uint32_t webGuardRefused() {
    using namespace espurna::web::guard;
    return state ? state->refused : 0;
}

void webGuardSetup() {
    espurna::web::guard::setup();
}

#endif // WEB_SUPPORT
