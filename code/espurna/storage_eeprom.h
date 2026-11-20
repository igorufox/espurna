/*

EEPROM MODULE

Settings live in a 4 KiB RAM buffer, Embedis writes them from the end towards the beginning. The buffer is committed
from the loop as a whole into a pool of flash sectors (storage_eeprom.cpp, one code for every chip, the format of
libs/settings_rotate.h: EEPROM_Rotate's, the ESP8266 boards keep theirs). The chip part is only its flash and where the
pool is, arch/<chip>/storage_flash_<chip>.cpp (ref. espurna::storage::flash below)

*/

#pragma once

#include "text_types.h"


#if __has_include(<spi_flash_mmap.h>)
#include <spi_flash_mmap.h>    // ESP-IDF 5 (core 3.x): SPI_FLASH_SEC_SIZE
#endif

// the chip's (ESP8266: spi_flash.h of its SDK, arch/esp8266/compat_chip.h)
#include "compat_chip.h"
#ifndef SPI_FLASH_SEC_SIZE
#define SPI_FLASH_SEC_SIZE 4096    // LibreTiny
#endif

#include <cstddef>
#include <cstdint>

// "The library uses 3 bytes to track last valid sector, so there must be at least 3"
constexpr auto EepromRotateReservedSize = size_t{ 3 };

// These are reserved by ESPurna internals. Currently unused, but kept for backwards compatibility.
constexpr auto EepromRotateOffset = size_t{ 11 };

// Always offset by this much when dealing with raw dataptr from EEPROM storage.
constexpr auto EepromReservedSize
    = size_t{ EepromRotateOffset + EepromRotateReservedSize };

// The whole flash sector, offset & internal bytes included
constexpr auto EepromSize = size_t{ SPI_FLASH_SEC_SIZE };

class StorageEEPROM_Rotate {
public:
    StorageEEPROM_Rotate() = default;

    StorageEEPROM_Rotate(StorageEEPROM_Rotate&) = delete;
    StorageEEPROM_Rotate& operator=(StorageEEPROM_Rotate&) = delete;

    StorageEEPROM_Rotate(StorageEEPROM_Rotate&&) = delete;
    StorageEEPROM_Rotate& operator=(StorageEEPROM_Rotate&&) = delete;

    const uint8_t* data() const;
    uint8_t* data();

    void setDirty();

    // fill all of the available storage with the 'value', reserved bytes stay as they are
    void fill(uint8_t value);

    uint8_t read(size_t address);
    void write(size_t address, uint8_t value);
    bool commit();

    void dump(::espurna::Output& print) const;

    // flash sectors of the settings: the base one and how many sectors the copies take under it (esp8266 EEPROM_Rotate)
    uint32_t base() const;
    uint8_t size() const;

    template <typename T>
    T& get(size_t address, T& data) {
        auto* ptr = reinterpret_cast<uint8_t*>(&data);
        for (size_t n = 0; n < sizeof(T); ++n) {
            ptr[n] = read(address + n);
        }
        return data;
    }

    template <typename T>
    void put(size_t address, const T& data) {
        const auto* ptr = reinterpret_cast<const uint8_t*>(&data);
        for (size_t n = 0; n < sizeof(T); ++n) {
            write(address + n, ptr[n]);
        }
    }
};

// The flash of the chip under the one storage of storage_eeprom.cpp (arch/<chip>/storage_flash_<chip>.cpp): where the
// pool of settings sectors is and how a sector gets read, erased, written. The format is libs/settings_rotate.h
namespace espurna {
namespace storage {
namespace flash {

struct Pool {
    uint32_t base;   // flash sector of the copy #0, the others below it
    uint8_t size;    // sectors in the pool; 0 - no storage, settings only live in RAM
};

Pool pool();

// whole sectors (EepromSize bytes, `data` 4-byte aligned)
bool read(uint32_t sector, uint8_t* data);
bool erase(uint32_t sector);
bool write(uint32_t sector, const uint8_t* data);

// a sector of the pool that must not be erased right now (Beken: the RF calibration block of the SDK, the slot the
// settings were taken over from until they are written in this format)
bool keep(uint32_t sector);

// No valid copy in the pool: the settings of the storage this chip had before (ESP32 the NVS blob, Beken its slots)
// into `data`, once; false - none. done() after they are written in this format
bool legacy(uint8_t* data);
void done();

// done() erases the former storage (ESP32): while it is still there, nothing was ever taken over from it, and a copy
// in the pool without a single key does not replace it. Beken: its slots linger in the pool region, ESP8266: none
bool legacyErasedWhenDone();

// EEPROM command, a line about the chip's storage
void info(::espurna::Output&);

} // namespace flash
} // namespace storage
} // namespace espurna

bool eepromReady();

void eepromRotate(bool value);

espurna::string eepromSectors();
size_t eepromSpace();

void eepromClear();
void eepromBackup(uint32_t index);

void eepromForceCommit(StorageEEPROM_Rotate&);
void eepromForceCommit();
void eepromCommit();

void eepromSetup();

StorageEEPROM_Rotate& eepromInstance();
