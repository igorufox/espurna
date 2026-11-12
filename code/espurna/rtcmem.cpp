/*

RTMEM MODULE

Copyright (C) 2019 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

Common part of every chip, ref. rtcmem.h for where the memory is

*/

#include "espurna.h"
#include "rtcmem.h"

#include <algorithm>
#include <cstring>

#if defined(ESP8266)
volatile RtcmemData* Rtcmem = reinterpret_cast<volatile RtcmemData*>(RTCMEM_ADDR);
#elif defined(ESP32)
// RTC_NOINIT_ATTR keeps the values across SW / WDT / panic resets and deep sleep
RTC_NOINIT_ATTR static RtcmemData _rtcmem_storage;
volatile RtcmemData* Rtcmem = &_rtcmem_storage;
#elif defined(LIBRETINY)
// No RAM survives the reset, bootloader clears all of it (0x00400000 and up, checked on BK7231N).
// Contents only live until the next reboot, relay state is restored from settings instead.
static RtcmemData _rtcmem_storage;
volatile RtcmemData* Rtcmem = &_rtcmem_storage;
#endif

namespace espurna {
namespace peripherals {
namespace {

namespace rtc {
namespace internal {

bool status = false;

} // namespace internal

constexpr uint32_t Magic { RTCMEM_MAGIC };

uintptr_t begin() {
    return RTCMEM_ADDR;
}

uintptr_t end() {
    return begin() + (4 * RTCMEM_BLOCKS);
}

void erase() {
    DEBUG_MSG_P(PSTR("[RTCMEM] Erasing start=0x%08x end=0x%08x\n"),
        begin(), end());

    auto first = reinterpret_cast<volatile uint32_t*>(begin());
    auto last = reinterpret_cast<volatile uint32_t*>(end());
    for (auto it = first; it != last; ++it) {
        *it = 0;
    }
}

void init() {
    erase();
    Rtcmem->magic = Magic;
}

// Treat memory as dirty on cold boot, hardware wdt reset and rst pin.
// Beken: nothing but the system word survives any reset (ref. rtcmemChipSetup())
bool status() {
#if defined(LIBRETINY)
    return false;
#else
    switch (systemResetReason()) {
    case REASON_EXT_SYS_RST:
    case REASON_DEFAULT_RST:
        return false;
    default:
        break;
    }

    return Magic == Rtcmem->magic;
#endif
}

#if TERMINAL_SUPPORT
namespace terminal {

PROGMEM_STRING(Init, "RTCMEM.INIT");

void init(::terminal::CommandContext&& ctx) {
    rtc::init();
    terminalOK(ctx);
}

PROGMEM_STRING(Dump, "RTCMEM.DUMP");

void dump(::terminal::CommandContext&& ctx) {
    ctx.output.printf_P(PSTR("boot_status=%s status=%s\n"),
        internal::status ? "OK" : "INIT",
        status() ? "OK" : "INIT");

    constexpr size_t BytesPerBlock = sizeof(uint32_t);
    constexpr size_t BlocksPerLine = 8;

    alignas(4) uint8_t buffer[BytesPerBlock * BlocksPerLine];
    String line;

    const auto last = end();
    for (auto addr = begin(); addr < last; addr += std::size(buffer)) {
        const auto size = std::min<size_t>(std::size(buffer), last - addr);
        std::memcpy(&buffer[0], reinterpret_cast<const uint32_t*>(addr), size);

        line += PSTR("0x");
        line += String(addr, 16);
        line += ':';

        for (auto it = std::begin(buffer); it != std::begin(buffer) + size; it += BytesPerBlock) {
            line += PSTR(" ");
            line += hexEncode(it, it + BytesPerBlock);
        }

        line += '\n';

        ctx.output.print(line);
        line = "";
    }
}

static constexpr ::terminal::Command Commands[] PROGMEM {
    {Init, init},
    {Dump, dump},
};

void setup() {
    espurna::terminal::add(Commands);
}

} // namespace terminal
#endif

bool current_status() {
    return internal::status;
}

void setup() {
#if TERMINAL_SUPPORT
    terminal::setup();
#endif

    internal::status = status();
    if (!internal::status) {
        init();
    }

#if defined(LIBRETINY)
    rtcmemChipSetup();
#endif
}

} // namespace rtc

} // namespace
} // namespace peripherals
} // namespace espurna

bool rtcmemStatus() {
    return espurna::peripherals::rtc::current_status();
}

void rtcmemSetup() {
    espurna::peripherals::rtc::setup();
}
