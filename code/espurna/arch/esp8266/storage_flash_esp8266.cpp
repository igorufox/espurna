/*

SETTINGS STORAGE, the ESP8266 flash on ESP8266_RTOS_SDK 3.4 (the storage itself is storage_eeprom.cpp, one for every chip)

The pool is where the NONOS build keeps it (owner 2026-11-17: «250/251»), the 1 MB layout on every chip size: sector 251
(0xFB000) and its copy 250 (0xFA000), the 'settings' partition of arch/esp8266/partitions.csv. A board moved from
the NONOS build keeps its settings, the format is the same

*/

#include "espurna.h"

#include "storage_eeprom.h"

#include <spi_flash.h>

namespace espurna {
namespace storage {
namespace flash {
namespace {

constexpr uint32_t Base { 251 };
constexpr uint8_t Sectors { 2 };

} // namespace

Pool pool() {
    return Pool{ Base, Sectors };
}

bool read(uint32_t sector, uint8_t* data) {
    return spi_flash_read(sector * SPI_FLASH_SEC_SIZE, data, SPI_FLASH_SEC_SIZE) == ESP_OK;
}

bool erase(uint32_t sector) {
    return spi_flash_erase_sector(sector) == ESP_OK;
}

bool write(uint32_t sector, const uint8_t* data) {
    return spi_flash_write(sector * SPI_FLASH_SEC_SIZE, data, SPI_FLASH_SEC_SIZE) == ESP_OK;
}

bool keep(uint32_t) {
    return false;
}

// the format is the one these boards always had
bool legacy(uint8_t*) {
    return false;
}

void done() {
}

bool legacyErasedWhenDone() {
    return false;
}

void info(::espurna::Output& out) {
    out.printf_P(PSTR("Flash: %u KiB, settings in sectors %u and %u\n"),
        spi_flash_get_chip_size() / 1024, Base, Base - 1);
}

} // namespace flash
} // namespace storage
} // namespace espurna
