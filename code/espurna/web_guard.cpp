/*

WEB GUARD

New TCP connections to the web server and telnet wait while the heap is low. The same code on every chip.

Every request of the web server holds several KB until its answer is sent (the request, the response buffer, the
queued TCP data). Parallel requests ran the heap out:
- BK7231N running Matter (27.6 KB free): 15 parallel `GET /` took it to 0, the loop stood for 8.5 s, watchdog restart
  (10.0.5.139, 2026-11-09)
- ESP8266 1 MB head with modules (4-9 KB free): two WebUI tabs, failed allocations and an OOM abort in the server's
  response (10.0.5.0, 2026-11-09). Clients that time out and ask again pile up requests until the heap is gone.
- ESP8266 1 MB head with 10 modules: 3 connections at a time (one of them the WebSocket) still took ~12 KB, oom counts
  in the hundreds per round of 12 parallel requests; the resets made the WebUI's part fetches fail, so a second tab
  never showed its relay panel (10.0.5.0, 2026-11-16, W64 / W70)

A gate sits between lwIP and the server: the accept callback of the listening pcb (lpcb->accept) of the web server
and of telnet. A new connection is not taken when
- its port already has webGuardConns connections (the new one and web sockets included),
- the free heap is below webGuardHeap,
- the largest free block is below webGuardBlock,
- lwIP's own heap (where lwIP has one, MEM_LIBC_MALLOC 0: the BK7231N) has less than webGuardLwip free.
0 turns a check off; defaults per chip: espurna::chip::web_guard() (espurna_chip.h). Connections already open are never
closed (the WebUI stays connected): the guard recovers by itself, the heap comes back as they finish.

Not taken means waiting, not failing: once a new connection would not be taken, the listener's backlog goes to 0
(lwIP with TCP_LISTEN_BACKLOG drops the SYNs of new connections, nothing is allocated for them) and the client sends its
SYN again after 1, 2, 4 ... s - the browser's request waits instead of failing. The loop opens the backlog again as soon
as one more would be taken. webGuardReqs is not a gate (20.11: the dropped SYNs made the WebUI's parallel parts wait
seconds): the web server handles that many requests at once and takes the next accepted connection from its queue. A
connection that got through the handshake anyway (it was under way when the backlog closed) is reset at once, before
the server allocates anything for it.

No server or library code changes: the loop puts the gate on the listeners every few seconds (a server started later
or again gets it as well). lwIP's pcb lists are walked in lwIP's context: through the tcpip thread with an OS, directly
on the ESP8266 NONOS SDK (NO_SYS). The gate itself runs in lwIP's context, thresholds are copied there from the
settings by the loop.

Chip specific: the free heap and the largest block come from systemFreeHeap() / systemHeapStats() (arch/<chip>/
system_<chip>.cpp). The ESP8266 head keeps its SDK reserve in arch/esp8266/heap_reserve_esp8266.cpp.

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

#include "libs/lwip_call.h"

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

uint32_t requests() {
    return espurna::chip::web_guard().requests;
}

// how often the loop looks for listeners without the gate
constexpr uint32_t ScanInterval { 5000 };

// how often the loop looks whether a held listener takes connections again
constexpr uint32_t HoldInterval { 50 };

} // namespace build

namespace settings {
namespace keys {

STRING_VIEW_INLINE(Heap, "webGuardHeap");
STRING_VIEW_INLINE(Block, "webGuardBlock");
STRING_VIEW_INLINE(Lwip, "webGuardLwip");
STRING_VIEW_INLINE(Conns, "webGuardConns");
STRING_VIEW_INLINE(Requests, "webGuardReqs");

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

uint32_t requests() {
    return getSetting(keys::Requests, build::requests());
}

} // namespace settings

// the original accept callback of a listener, by its port (set once, the listeners are made once at boot)
struct Listener {
    void* arg;
    tcp_accept_fn accept;
    uint16_t port;
    uint8_t backlog;    // the listener's own, what an open listener gets back
    bool web;           // the web server's: webGuardReqs counts its requests
    bool held;          // backlog 0, SYNs of new connections dropped
};

// allocated at setup: no static RAM on the ESP8266
struct State {
    Listener listeners[2];
    uint32_t heap;
    uint32_t block;
    uint32_t lwip;
    uint32_t conns;
    uint32_t requests;
    uint32_t refused;
    uint32_t reported;
    uint32_t holds;
    uint32_t holds_reported;
    uint32_t scanned;
    uint32_t checked;
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

// `incoming`: the new connection is in lwIP's list already (the gate), else: would one more be taken now
bool admit(const State& guard, const Listener& listener, bool incoming) {
    if (guard.conns && ((connections(listener.port) + (incoming ? 0 : 1)) > guard.conns)) {
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

// handshakes the listener may have under way: its own backlog, or the free request slots of the web server
uint8_t slots(const State& guard, const Listener& listener) {
    (void)guard;
    const uint32_t out = listener.backlog ? listener.backlog : 1;
    return static_cast<uint8_t>(out ? out : 1);
}

err_t gate(void* arg, struct tcp_pcb* pcb, err_t err);

// the backlog of the listening pcb: 0 drops the SYNs of new connections (the client sends them again), nothing is
// allocated for them. Without TCP_LISTEN_BACKLOG the gate resets what it does not take
void setBacklog(State& guard, Listener& listener, bool open) {
#if TCP_LISTEN_BACKLOG
    for (auto* lpcb = tcp_listen_pcbs.listen_pcbs; lpcb; lpcb = lpcb->next) {
        if ((lpcb->local_port == listener.port) && (lpcb->accept == gate)) {
            lpcb->backlog = open ? slots(guard, listener) : 0;
        }
    }

    if (!open && !listener.held) {
        ++guard.holds;
    }
    listener.held = !open;
#else
    (void)guard;
    (void)listener;
    (void)open;
#endif
}

err_t gate(void* arg, struct tcp_pcb* pcb, err_t err) {
    auto& guard = *state;

    // lwIP passes no pcb when it could not make one (ERR_MEM)
    Listener* listener = nullptr;
    for (auto& entry : guard.listeners) {
        if (entry.accept && (pcb ? (entry.port == pcb->local_port) : (entry.arg == arg))) {
            listener = &entry;
            break;
        }
    }

    // reset before the server allocates anything; the next ones wait in TCP until the loop opens the listener again
    if (pcb && (ERR_OK == err) && (!listener || !admit(guard, *listener, true))) {
        ++guard.refused;
        tcp_abort(pcb);
        if (listener) {
            setBacklog(guard, *listener, false);
        }
        return ERR_ABRT;
    }

    if (!listener) {
        return ERR_VAL;
    }

    const auto result = listener->accept(arg, pcb, err);

    // taken (the listener's accept() allocated its part already): would one more be taken now?
    setBacklog(guard, *listener, admit(guard, *listener, false));

    return result;
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
#if TCP_LISTEN_BACKLOG
                listener.backlog = lpcb->backlog;
#else
                listener.backlog = 1;
#endif
                listener.held = false;
                lpcb->accept = gate;
                break;
            }
        }
    }
}

// a held listener takes connections again once one more would be taken
void reopen() {
    auto& guard = *state;
    for (auto& listener : guard.listeners) {
        if (listener.accept && listener.held && admit(guard, listener, false)) {
            setBacklog(guard, listener, true);
        }
    }
}

// in lwIP's context, the one way of every chip (libs/lwip_call.h: the tcpip thread, or the loop without an OS)
void installInLwip() {
    ::espurna::lwip::lwipCall([]() {
        install();
    });
}

void reopenInLwip() {
    ::espurna::lwip::lwipCall([]() {
        reopen();
    });
}

// --- loop ----------------------------------------------------------------------------------------------------------

void configure() {
    auto& guard = *state;
    guard.heap = settings::heap();
    guard.block = settings::block();
    guard.lwip = settings::lwip();
    guard.conns = settings::conns();
    guard.requests = settings::requests();
    // the request count is the web server's own (a queue of accepted connections), no longer a gate on the SYNs:
    // dropped SYNs came again after 1 s, 3 s... - the WebUI's parallel parts loaded seconds slower than on NONOS (20.11)
    webServer().setConcurrency(guard.requests);
}

bool held() {
    for (const auto& listener : state->listeners) {
        if (listener.held) {
            return true;
        }
    }

    return false;
}

void loop() {
    auto& guard = *state;

    const auto now = espurna::time::ms();
    if (held() && (now - guard.checked >= build::HoldInterval)) {
        guard.checked = now;
        reopenInLwip();
    }

    if (now - guard.scanned < build::ScanInterval) {
        return;
    }

    guard.scanned = now;
    installInLwip();

    const auto refused = guard.refused;
    const auto holds = guard.holds;
    if ((refused != guard.reported) || (holds != guard.holds_reported)) {
        DEBUG_MSG_P(PSTR("[WEBSERVER] Low heap guard: %u new connection(s) reset, held %u time(s), free heap %u\n"),
            static_cast<unsigned>(refused - guard.reported),
            static_cast<unsigned>(holds - guard.holds_reported),
            static_cast<unsigned>(systemFreeHeap()));
        guard.reported = refused;
        guard.holds_reported = holds;
    }
}

void setup() {
    state = new State{};

    state->listeners[0].port = webPort();
    state->listeners[0].web = true;
#if TELNET_SUPPORT
    state->listeners[1].port = telnetPort();
#endif

    configure();
    installInLwip();
    state->scanned = espurna::time::ms();

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
