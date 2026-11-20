/*

What arch/beken calls of the chip besides chip_sdk.h, on the BDK (core_beken.cpp): the flash, the heap, the watchdog,
the reboot reason, the MAC of the RF calibration block. Was LibreTiny's lt_* API until 2026-11-19

*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace beken {

// the flash device itself, any offset below flash_size(): byte count done (0 on an error)
uint32_t flash_size();
uint32_t flash_id();
uint32_t flash_read(uint32_t offset, uint8_t* data, size_t length);
uint32_t flash_write(uint32_t offset, const uint8_t* data, size_t length);
// the 4 KiB sectors over [offset, offset + length)
bool flash_erase(uint32_t offset, size_t length);
bool flash_erase_block(uint32_t offset);

uint32_t heap_free();
uint32_t heap_min_free();
uint32_t heap_max_alloc();
uint32_t heap_size();

enum class Reboot {
    Unknown,
    Power,
    Software,
    Watchdog,
    Crash,
    SleepGpio,
    SleepRtc,
    SleepUsb,
};

Reboot reboot_reason();
const char* reboot_reason_name(Reboot);
[[noreturn]] void reboot();

// the hardware watchdog, timeout in ms. rtcmem_beken.cpp: the stall watcher learns the period and the feeds, then
// the raw calls of core_beken.cpp
void wdt_enable(uint32_t timeout);
void wdt_disable();
void wdt_feed();

namespace raw {

void wdt_enable(uint32_t timeout);
void wdt_disable();
void wdt_feed();

} // namespace raw

// the station MAC of the RF calibration block (as the BDK reads it at the start)
void device_mac(uint8_t* mac);

void rand_bytes(uint8_t* out, size_t length);

// the TRNG of the chip (the entropy of mbedTLS: mbedtls_hardware_poll of core_beken.cpp)
void trng_bytes(uint8_t* out, size_t length);

const char* cpu_model();
uint32_t cpu_mhz();
uint32_t cpu_cycle_count();
const char* bdk_version();

} // namespace beken
} // namespace espurna
