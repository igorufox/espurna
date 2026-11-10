/*

EEPROM MODULE

Settings live in a 4 KiB RAM buffer, Embedis writes them from the end towards the beginning. The buffer is committed
from the loop as a whole (storage_eeprom.cpp). Where the buffer is kept and how it gets to the flash is the chip part,
arch/<chip>/storage_eeprom_<chip>.cpp (ref. espurna::storage::chip below)

*/

#pragma once

#include <Arduino.h>

#if __has_include(<spi_flash_mmap.h>)
#include <spi_flash_mmap.h>    // ESP-IDF 5 (core 3.x): SPI_FLASH_SEC_SIZE
#elif defined(ESP8266)
#include <spi_flash.h>
#endif
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

    void dump(Print& print) const;

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

// Chip part, arch/<chip>/storage_eeprom_<chip>.cpp. What both FreeRTOS chips do the same way is in
// arch/rtos/storage_eeprom_rtos.cpp
namespace espurna {
namespace storage {
namespace chip {

// RAM copy of the settings, EepromSize bytes
uint8_t* buffer();

// the saved settings into buffer() from eepromSetup(); false when there is no storage at all, settings only live in RAM
bool load();

// settings are loaded on the first use (esp8266 EEPROM_Rotate), commits work before eepromSetup() as well
bool early();

// the buffer was changed
void changed();

// buffer() to the flash
bool save();

// a failed save() is tried again from the loop every few seconds. Not on esp8266, every attempt erases a sector there
bool retry();

// chip specific terminal commands
void setup();

// EEPROM terminal command, eepromSectors(), eepromSpace(), StorageEEPROM_Rotate::dump()
void info(Print&, bool dirty);
String sectors();
size_t space();
void dump(Print&);

// flash sectors, ref. StorageEEPROM_Rotate::base() and size()
uint32_t base();
uint8_t pool();

// eepromRotate(), true when the storage has to be committed. eepromBackup()
bool rotate(bool value);
void backup(uint32_t index);

} // namespace chip
} // namespace storage
} // namespace espurna

bool eepromReady();

void eepromRotate(bool value);

String eepromSectors();
size_t eepromSpace();

void eepromClear();
void eepromBackup(uint32_t index);

void eepromForceCommit(StorageEEPROM_Rotate&);
void eepromForceCommit();
void eepromCommit();

void eepromSetup();

StorageEEPROM_Rotate& eepromInstance();
