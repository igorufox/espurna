/*

PROTOTYPE: heap protection of the ESP8266 1 MB head image with modules (docs/modules.md)

With the modules running the free heap is 4-9 KB. A WebUI visit (the page, its WebSocket, the MODULES panel, two
tabs) runs it out: 10.0.5.0 counted 45 failed allocations for two tabs (2026-11-09). Most callers live with a NULL,
the NONOS SDK does not: it wrote through one while it parsed a beacon (Exception 29, memcpy to 0 in
ieee80211_setup_ratetable), twice while the user switched sensors off in the MODULES panel.

- The SDK reserve: a block kept aside at boot. The head links with --wrap of the allocation calls the SDK makes
  (pvPortZalloc, sdk3_pvPortMalloc, pvPortRealloc; scripts/module_head.py, custom_head_wraps): when one fails, the block is
  freed and the call tried again, so the SDK gets its memory. The loop takes the block back once the heap is there.
- operator new / new[] (the core aborts when malloc fails) get the reserve the same way.
- New TCP connections wait while the heap is low: the common guard of the web server and telnet, web_guard.cpp.

*/

#include "espurna.h"

#if MODULES_SUPPORT

#include "modules.h"
#include "web_guard.h"

#include <lwip/tcp.h>

#ifndef MODULES_SDK_RESERVE
#define MODULES_SDK_RESERVE 1536
#endif

extern "C" {
void* __real_pvPortZalloc(size_t size, const char* file, int line);
void* __real_sdk3_pvPortMalloc(size_t size, const char* file, int line);
void* __real_pvPortRealloc(void* ptr, size_t size, const char* file, int line);
void* __real__Znwj(size_t size);
void* __real__Znaj(size_t size);
}

namespace espurna {
namespace modules {
namespace heap {
namespace {

void* reserve { nullptr };
uint32_t reserve_used { 0 };
bool told { true };

bool IRAM_ATTR release() {
    void* block = reserve;
    if (!block) {
        return false;
    }

    reserve = nullptr;
    ++reserve_used;
    free(block);
    return true;
}

void take() {
    if (!reserve && (ESP.getFreeHeap() > (MODULES_SDK_RESERVE + 4096))) {
        reserve = malloc(MODULES_SDK_RESERVE);
    }
}

// the free heap the loop sees (not inside a request, as /modules does): the last value and the lowest of the
// last one to two minutes
uint32_t heap_last { 0 };
uint32_t heap_low_now { 0xffffffff };
uint32_t heap_low_before { 0xffffffff };
uint32_t heap_window { 0 };
uint32_t heap_sampled { 0 };

void sampleHeap() {
    const auto now = millis();
    if (now - heap_sampled < 1000) {
        return;
    }
    heap_sampled = now;
    heap_last = ESP.getFreeHeap();
    heap_low_now = std::min(heap_low_now, heap_last);
    if (now - heap_window >= 60000) {
        heap_window = now;
        heap_low_before = heap_low_now;
        heap_low_now = heap_last;
    }
}

void loop() {
    sampleHeap();
    if (!reserve && !told) {
        told = true;
        DEBUG_MSG_P(PSTR("[MODULES] Out of heap, the SDK got the reserve (%u times)\n"), reserve_used);
    }

    if (!reserve) {
        take();
        if (reserve) {
            told = false;
        }
    }
}

} // namespace
} // namespace heap
} // namespace modules
} // namespace espurna

extern "C" {

void* IRAM_ATTR __wrap_pvPortZalloc(size_t size, const char* file, int line) {
    void* out = __real_pvPortZalloc(size, file, line);
    if (!out && espurna::modules::heap::release()) {
        out = __real_pvPortZalloc(size, file, line);
    }
    return out;
}

void* IRAM_ATTR __wrap_sdk3_pvPortMalloc(size_t size, const char* file, int line) {
    void* out = __real_sdk3_pvPortMalloc(size, file, line);
    if (!out && espurna::modules::heap::release()) {
        out = __real_sdk3_pvPortMalloc(size, file, line);
    }
    return out;
}

void* IRAM_ATTR __wrap_pvPortRealloc(void* ptr, size_t size, const char* file, int line) {
    void* out = __real_pvPortRealloc(ptr, size, file, line);
    if (!out && size && espurna::modules::heap::release()) {
        out = __real_pvPortRealloc(ptr, size, file, line);
    }
    return out;
}

// operator new / new[]: the core aborts when malloc fails ("OOM", restart reason 254: 10.0.5.0 in the web server's
// response, 2026-11-09); the reserve is tried first
void* __wrap__Znwj(size_t size) {
    void* out = malloc(size ? size : 1);
    if (!out && espurna::modules::heap::release()) {
        out = malloc(size ? size : 1);
    }
    return out ? out : __real__Znwj(size);
}

void* __wrap__Znaj(size_t size) {
    void* out = malloc(size ? size : 1);
    if (!out && espurna::modules::heap::release()) {
        out = malloc(size ? size : 1);
    }
    return out ? out : __real__Znaj(size);
}

} // extern "C"

void modulesHeapSetup() {
    espurna::modules::heap::take();
    espurna::modules::heap::told = false;
    espurnaRegisterLoop(espurna::modules::heap::loop);
}

uint32_t modulesHeapReserve() {
    return espurna::modules::heap::reserve ? MODULES_SDK_RESERVE : 0;
}

uint32_t modulesHeapReserveUsed() {
    return espurna::modules::heap::reserve_used;
}

uint32_t modulesAcceptRefused() {
#if WEB_SUPPORT
    return webGuardRefused();
#else
    return 0;
#endif
}

uint32_t modulesHeapLoop() {
    return espurna::modules::heap::heap_last;
}

uint32_t modulesHeapLow() {
    using namespace espurna::modules::heap;
    const auto out = std::min(heap_low_now, heap_low_before);
    return (out == 0xffffffff) ? heap_last : out;
}

#endif // MODULES_SUPPORT
