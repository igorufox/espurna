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

#include <cstdint>

// Change this when modifying RtcmemData
#define RTCMEM_MAGIC 0x46535078

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
    // the last espota upload (ota_arduinoota.cpp): its tag while it runs, the bytes taken of its size
    uint32_t ota_trace;
    uint32_t ota_bytes;
    uint32_t ota_size;
    // the AP of the last connection (wifi.cpp): the first attempt after a reset goes straight to it, no scan of every
    // channel (owner 19.11: 2 s of the 6.5 s connect); `ssid` is a hash of the network's SSID, 0 - nothing kept
    // whole words only: the RTC memory of the ESP8266 takes no byte stores (a byte store wrote its byte into every
    // byte of the word, 19.11). bssid: bytes 0..3 in bssid_lo, 4..5 in the low half of bssid_hi; channel in bits 16..23
    // of bssid_hi, bit 31 - the last attempt to it failed
    struct {
        uint32_t ssid;
        uint32_t bssid_lo;
        uint32_t bssid_hi;
    } wifi;
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
