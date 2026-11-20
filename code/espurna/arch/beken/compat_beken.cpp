/*

esp8266 Core API shims, BK72xx (BDK) part (ref. compat_chip.h)

*/

#include "../../espurna.h"

#include "../../rtcmem.h"

#include "beken_api.h"

#include <lwip/sys.h>
#include <lwip/tcpip.h>

#include <cstdlib>
#include <new>


// __atomic_* of the ARM968E-S (no atomic instructions, no libatomic): only the prebuilt Matter library needs them,
// its port has them (matter-espurna bk7231n_port.c); ESPurna has no std::atomic (espurna::system::Shared)

// beken::reboot_reason_name() names what the SDK start type says, which is 'watchdog' after our crash handlers
espurna::string EspCompat::getResetReason() {
    switch (rtcmemCrash()) {
    case RtcmemCrash::None:
        break;
    case RtcmemCrash::Stall:
        return espurna::string("Watchdog");
    default:
        return espurna::string("Crash");
    }

    return espurna::string(::espurna::beken::reboot_reason_name(::espurna::beken::reboot_reason()));
}

espurna::string EspCompat::getResetInfo() {
    switch (rtcmemCrash()) {
    case RtcmemCrash::None:
        break;
    case RtcmemCrash::Undefined:
        return espurna::string("Crash (undefined instruction)");
    case RtcmemCrash::PrefetchAbort:
        return espurna::string("Crash (prefetch abort)");
    case RtcmemCrash::DataAbort:
        return espurna::string("Crash (data abort)");
    case RtcmemCrash::Reserved:
        return espurna::string("Crash (reserved exception)");
    case RtcmemCrash::MallocFailed:
        return espurna::string("Crash (out of memory)");
    case RtcmemCrash::Stall:
        return espurna::string("Loop stalled (watchdog)");
    }

    return espurna::string(::espurna::beken::reboot_reason_name(::espurna::beken::reboot_reason()));
}

uint32_t EspCompat::getFlashChipSpeed() {
    return ::espurna::core::chip::flash_speed();
}

uint32_t EspCompat::getFlashChipId() {
    return ::espurna::beken::flash_id();
}

uint32_t EspCompat::getFreeSketchSpace() {
    return ::espurna::core::chip::sketch_space();
}

uint32_t EspCompat::getSketchSize() {
    return ::espurna::core::chip::sketch_size();
}

uint32_t EspCompat::getMaxAllocHeap() {
    return ::espurna::beken::heap_max_alloc();
}

bool EspCompat::flashRead(uint32_t addr, uint32_t* data, size_t size) {
    return ::espurna::beken::flash_read(addr, reinterpret_cast<uint8_t*>(data), size) == size;
}



// new (std::nothrow) is the common one of ESP32 and BK72xx, arch/rtos/system_rtos.cpp (W1: the tcpip thread spun in
// abort <- operator new <- new[](nothrow) of libstdc++ with the heap short, 10.0.5.139 2026-11-14)

// The lwIP core lock (LOCK_TCPIP_CORE, tcpip.c) without priority inheritance: a binary semaphore instead of the
// FreeRTOS mutex of the BDK's sys_mutex_new(). With the mutex the Matter task (priority 5) took the core lock for its
// UDP endpoints (EndPointStateLwIP::RunOnTCPIP), the tcpip thread waited on it and lent the Matter task its 7. The
// FreeRTOS V9 of the BDK gives a task its own priority back only when it holds no mutex at all, and the Matter task
// holds the lock of its stack through a whole event: the server init (~25 s of certificate crypto) then ran at 7, the
// loop (6) got no CPU and the watchdog reset the board ~10 s after Matter started (10.0.5.139 2026-11-15, the stall
// record: CHIP 7/5, tcp/ip idle on its mailbox).
// The same goes for the other lwIP mutex, mem_mutex of mem.c (MEM_LIBC_MALLOC 0, LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT
// 0): mem_malloc() and mem_free() take it from every task. The Matter task allocates and frees its PBUF_RAM packet
// buffers itself (SystemPacketBuffer pbuf_alloc / pbuf_free, outside the core lock), tcp/ip and core_thr (both 7) do the
// same for every packet; a collision lent the Matter task 7 again and it kept it for the rest of the event (W74,
// 10.0.5.79 2026-11-16: the stall record had CHIP 7/5 and nobody waiting on a mutex any more, the loan was already
// over). So every lwIP mutex is a binary semaphore: all of them are held for a few lines of lwIP code only
extern "C" {

err_t __wrap_sys_mutex_new(sys_mutex_t* mutex) {
    *mutex = xSemaphoreCreateBinary();
    if (!*mutex) {
        return ERR_MEM;
    }

    xSemaphoreGive(*mutex);
    return ERR_OK;
}

} // extern "C"
