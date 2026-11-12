/*

RTMEM MODULE

Copyright (C) 2019 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

Memory that survives a software reset: relay and light state, energy totals, the system word (custom reset reason and
the stability counter), the pins kept through the reset. One structure on every chip, implemented by rtcmem.cpp.
Where it lives is the chip's:
- esp8266, the user part of the RTC memory at a fixed address
- ESP32, RTC_NOINIT memory
- Beken, plain RAM (the bootloader clears all of it); only the system word survives, in the retention register
  (ref. arch/beken/rtcmem_beken.cpp)

*/

#pragma once

#include <Arduino.h>
#include <cstdint>

// Change this when modifying RtcmemData
#define RTCMEM_MAGIC 0x46535076

struct RtcmemEnergy {
    uint32_t kwh;
    uint32_t ws;
};

struct RtcmemData {
    uint32_t magic;
    uint32_t sys;
    uint32_t relay;
    uint32_t mqtt;
    uint64_t light;
    RtcmemEnergy energy[4];
    uint32_t gpio_ignore;
};

extern volatile RtcmemData* Rtcmem;

#if defined(ESP8266)

// Base address of USER RTC memory
// https://github.com/esp8266/esp8266-wiki/wiki/Memory-Map#memmory-mapped-io-registers
#define RTCMEM_ADDR_BASE (0x60001200)

// RTC memory is accessed using blocks of 4 bytes.
// Blocks 0..63 are reserved by the SDK, 64..192 are available to the user.
// Blocks 64..96 are reserved by the eboot 'struct eboot_command' (128 -> (128 / 4) -> 32):
// https://github.com/esp8266/Arduino/blob/master/bootloaders/eboot/eboot_command.h
#define RTCMEM_OFFSET 32u
#define RTCMEM_ADDR (RTCMEM_ADDR_BASE + (RTCMEM_OFFSET * 4u))

#define RTCMEM_BLOCKS 96u

#else

// The structure itself is all there is (rpn mem_read / mem_write, RTCMEM.DUMP)
#define RTCMEM_ADDR (reinterpret_cast<uintptr_t>(Rtcmem))
#define RTCMEM_BLOCKS (sizeof(RtcmemData) / 4u)

#endif

static_assert(sizeof(RtcmemData) <= (RTCMEM_BLOCKS * 4u), "RTCMEM struct is too big");

bool rtcmemStatus();
void rtcmemSetup();

#if defined(LIBRETINY)

// Only Rtcmem->sys survives a reset here (ref. rtcmem_beken.cpp)
bool rtcmemSysStatus();
uint32_t rtcmemRetention();

// rtcmemSetup(), once the structure is initialized: the system word of the previous boot, the crash record place
void rtcmemChipSetup();

// Exception that caused the last reset, written into the register by our crash handlers
enum class RtcmemCrash : uint32_t {
    None = 0,
    Undefined,
    PrefetchAbort,
    DataAbort,
    Reserved,
    // Only in the flash record (written by the malloc failed hook), never in the register
    MallocFailed,
    // Only in the flash record: the watchdog was about to reset the chip, written by the stall watcher with the
    // saved context of every task
    Stall,
};

RtcmemCrash rtcmemCrash();

// Registers and stack of the last crash, kept in the flash (ref. rtcmem_beken.cpp). Setup reports a new one once,
// in the log and on MQTT, dump prints the whole record (CRASH command)
void rtcmemCrashSetup();
void rtcmemCrashDump(Print&);

#else

// Rtcmem->sys survives a reset with the rest of the RTCMEM
inline bool rtcmemSysStatus() {
    return rtcmemStatus();
}

#endif
