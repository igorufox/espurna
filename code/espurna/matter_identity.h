/*

MATTER: THE IDENTITY OF THE BOARD (ref. matter_identity.cpp), one code on ESP32 and Beken

*/

#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace matter {
namespace identity {

// The chip's part (arch/<chip>/matter_*.cpp): the key-value store of the chip, namespace "espurna-id" (ESP32 NVS,
// Beken FlashDB)
bool store_read(const char* name, uint8_t* out, size_t size, size_t& length);
bool store_write(const char* name, const uint8_t* data, size_t size);

// Factory data of a stock firmware ("dac-cert", "pai-cert", "cert-dclrn", "dac-pub-key", "dac-key", "vendor-id" ...),
// false when the board has none (every ESP32). A read without a buffer gives the size only
bool factory_read(const char* name, uint8_t* out, size_t size, size_t& length);

// Before the stack starts: the own identity read or made (once per board), its setup passcode and discriminator.
// false: the board has a factory identity (its codes come from the factory data) or the store failed
bool load(uint32_t& passcode, uint16_t& discriminator);

// Where the stack's providers are set: the factory or the own attestation, commissionable data and instance info
bool install();

bool factory();

// For the status command, no secrets
String status();

} // namespace identity
} // namespace matter
} // namespace espurna
