/*

esp8266 Core API shims, BK72xx (LibreTiny) part (ref. compat_chip.h)

*/

#include "espurna.h"

#include "rtcmem.h"

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
    if (rtcmemCrash() != RtcmemCrash::None) {
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

