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
#define RTCMEM_MAGIC 0x46535077

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
    // one bit per GPIO, 64 bits: the ESP32 has pins above 31 (relays on GPIO32/33)
    uint64_t gpio_ignore;
};

extern volatile RtcmemData* Rtcmem;

bool rtcmemStatus();
void rtcmemSetup();

// where the structure is (RTCMEM_ADDR, RTCMEM_BLOCKS), what of it survives a reset: the chip's (arch/<chip>/rtcmem_chip.h)
#include "rtcmem_chip.h"

static_assert(sizeof(RtcmemData) <= (RTCMEM_BLOCKS * 4u), "RTCMEM struct is too big");

// the chip's part of rtcmemSetup(), once the structure is initialized (Beken: the system word of the previous boot,
// the crash record place); and whether the structure survived the reset this boot came from
void rtcmemChipSetup();
bool rtcmemChipKept();
