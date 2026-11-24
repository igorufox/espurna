/*

MATTER: THE IDENTITY OF THE BOARD (ref. matter_identity.cpp), one code on ESP32 and Beken

*/

#pragma once


#include <cstddef>
#include <cstdint>

namespace espurna {
namespace matter {
namespace identity {

// The key-value store of the platform layer (matter_port.cpp, FlashDB on every chip), namespace "espurna-id"
bool store_read(const char* name, uint8_t* out, size_t size, size_t& length);
bool store_write(const char* name, const uint8_t* data, size_t size);

// Factory data of a stock firmware ("dac-cert", "pai-cert", "cert-dclrn", "dac-pub-key", "dac-key", "vendor-id" ...),
// false when the board has none (every ESP32). A read without a buffer gives the size only
bool factory_read(const char* name, uint8_t* out, size_t size, size_t& length);
// where the factory data is (the chip's part), empty without any
espurna::string factory_source();

// `matterIdentity` (owner 11-16): which identity the board uses. Not set: factory when the board has factory data, else
// espurna (the automatic choice before the setting)
constexpr char Setting[] = "matterIdentity";

enum class Choice : uint8_t {
    Factory,  // the factory data of the stock firmware
    Espurna,  // the own development identity made on the board
    Custom,   // the owner's DAC, key, PAI (+ declaration), uploaded in the MATTER panel
};

const char* name(Choice);
bool parse(const char* value, Choice& out);

// The module setup, in the loop before any start: reads `matterIdentity` (a choice the board cannot use falls back to
// espurna). An identity other than the one of the last start forgets the controllers (port::reset()), they verified
// the old DAC
void setup();

// In use (after the start) or chosen by setup()
Choice choice();

// The board has it: factory data, an uploaded custom identity (espurna always)
bool available(Choice);

// VID / PID of the uploaded custom DAC; false when there is none
bool custom_ids(uint16_t& vendor, uint16_t& product);

// VID / PID of the identity in use when the board made it (espurna, custom); false for the factory one
bool ids(uint16_t& vendor, uint16_t& product);

// From the loop: checks the uploaded custom identity (base64 of DER; the key DER SEC1 / PKCS#8 or the 32 raw bytes as
// the factory tables keep it; the declaration optional) and stores it. Checked: Matter DAC and PAI format, VID / PID in
// the DAC subject, the PAI signed the DAC, the key belongs to the DAC. false with the reason in `error`. The key is
// never logged or sent back. It does not switch to it: `matterIdentity custom` and a restart do
bool custom_store(const char* dac, const char* key, const char* pai, const char* declaration, espurna::string& error);

// Before the stack starts: the own identity read or made (once per board), its setup passcode and discriminator (the
// custom identity uses them too). false: the board has a factory identity (its codes come from the factory data) or the
// store failed
bool load(uint32_t& passcode, uint16_t& discriminator);

// Where the stack's providers are set: the factory, the own or the custom attestation, commissionable data and
// instance info
bool install();

bool factory();

// For the status command, no secrets
espurna::string status();

} // namespace identity
} // namespace matter
} // namespace espurna
