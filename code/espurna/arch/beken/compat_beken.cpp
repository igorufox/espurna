/*

esp8266 Core API shims, BK72xx (LibreTiny) part (ref. compat_chip.h)

*/

#include "espurna.h"

#include "rtcmem.h"

#include <lwip/sys.h>
#include <lwip/tcpip.h>

#include <cstdlib>
#include <new>

// 32bit micros() wraps every ~71 minutes, extend it. Called often enough (every loop())
// that a single wrap between the two calls is the only one possible
uint64_t micros64() {
    static uint32_t last { 0 };
    static uint32_t high { 0 };

    const auto level = port_disable_interrupts_flag();
    const uint32_t now = ::micros();
    if (now < last) {
        ++high;
    }
    last = now;
    const uint64_t out = (uint64_t{high} << 32) | now;
    port_enable_interrupts_flag(level);

    return out;
}

// __atomic_* of the ARM968E-S (no atomic instructions, no libatomic): only the prebuilt Matter library needs them,
// its port has them (matter-espurna bk7231n_port.c); ESPurna has no std::atomic (espurna::system::Shared)

// LibreTiny names what the SDK start type says, which is 'watchdog' after our crash handlers
String EspCompat::getResetReason() {
    switch (rtcmemCrash()) {
    case RtcmemCrash::None:
        break;
    case RtcmemCrash::Stall:
        return String(F("Watchdog"));
    default:
        return String(F("Crash"));
    }

    // the EspClass of LibreTiny, not ::ESP: here that is the macro of compat_chip.h, this very object
    return static_cast<EspClass&>(*this).getResetReason();
}

String EspCompat::getResetInfo() {
    switch (rtcmemCrash()) {
    case RtcmemCrash::None:
        break;
    case RtcmemCrash::Undefined:
        return String(F("Crash (undefined instruction)"));
    case RtcmemCrash::PrefetchAbort:
        return String(F("Crash (prefetch abort)"));
    case RtcmemCrash::DataAbort:
        return String(F("Crash (data abort)"));
    case RtcmemCrash::Reserved:
        return String(F("Crash (reserved exception)"));
    case RtcmemCrash::MallocFailed:
        return String(F("Crash (out of memory)"));
    case RtcmemCrash::Stall:
        return String(F("Loop stalled (watchdog)"));
    }

    return static_cast<EspClass&>(*this).getResetInfo();
}

uint32_t EspCompat::getFlashChipSpeed() {
    return 0;
}

uint32_t EspCompat::getFlashChipId() {
    return static_cast<EspClass&>(*this).getFlashChipId();
}

// LibreTiny OTA writes into the 'download' partition, image there is compressed
uint32_t EspCompat::getFreeSketchSpace() {
    return 0xA6000;
}

// Code and initialized data as linked, ref. LibreTiny bk72xx linker script (flash region starts at 0x10000)
extern "C" char _data_flash_begin;
extern "C" char _data_ram_begin;
extern "C" char _data_ram_end;

uint32_t EspCompat::getSketchSize() {
    constexpr uint32_t FlashOrigin { 0x10000 };
    const auto data = static_cast<uint32_t>(&_data_ram_end - &_data_ram_begin);
    return reinterpret_cast<uint32_t>(&_data_flash_begin) + data - FlashOrigin;
}

// EspClass::getMaxFreeBlockSize() of LibreTiny is 16bit
uint32_t EspCompat::getMaxAllocHeap() {
    return lt_heap_get_max_alloc();
}

bool EspCompat::flashRead(uint32_t addr, uint32_t* data, size_t size) {
    return lt_flash_read(addr, reinterpret_cast<uint8_t*>(data), size) == size;
}

// LibreTiny's ArduinoFamily.h makes noInterrupts() / interrupts() of these two, and ArduinoCore-API declares
// pulseIn(), none of them has a body on the BK72xx. The modules use them (OneWire, HLW8012): here, kept in the head
// by espurna/modules_exports.txt
extern "C" {

uint32_t ulPortSetInterruptMask(void) {
    return static_cast<uint32_t>(port_disable_interrupts_flag());
}

// interrupts() passes 0: enabled, the state the flag of an unmasked CPU holds
void vPortClearInterruptMask(uint32_t mask) {
    port_enable_interrupts_flag(static_cast<int>(mask));
}

// length of the next pulse at `state`, microseconds; 0 when it did not start or end within the timeout
unsigned long pulseIn(pin_size_t pin, uint8_t state, unsigned long timeout) {
    const auto level = [&]() {
        return (digitalRead(pin) == HIGH) ? HIGH : LOW;
    };
    const auto start = micros();
    const auto expired = [&]() {
        return (micros() - start) >= timeout;
    };

    // the pulse running now does not count
    while (level() == state) {
        if (expired()) {
            return 0;
        }
    }

    while (level() != state) {
        if (expired()) {
            return 0;
        }
    }

    const auto begin = micros();
    while (level() == state) {
        if (expired()) {
            return 0;
        }
    }

    return micros() - begin;
}

} // extern "C"


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
