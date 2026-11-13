/*

SETTINGS STORAGE, the ESP8266 flash (the storage itself is storage_eeprom.cpp, one for every chip)

The pool is where EEPROM_Rotate always had it, so the boards keep their settings: the base sector right under the 4
sectors of the SDK (flash size / 4 KiB - 5) and the copies under it, by the flash size (a 1 MB chip: 251 and 250).
EEPROM_ROTATE_SECTORS sets the pool size of a build

*/

#include "espurna.h"

#include "storage_eeprom.h"

namespace espurna {
namespace storage {
namespace flash {
namespace {

uint8_t pool_size(uint32_t base) {
#ifdef EEPROM_ROTATE_SECTORS
    static_assert(EEPROM_ROTATE_SECTORS > 0, "");
    return EEPROM_ROTATE_SECTORS;
#else
    if (base > 4000) { // 16 MB
        return 32;
    } else if (base > 2000) { // 8 MB
        return 16;
    } else if (base > 1000) { // 4 MB
        return 8;
    } else if (base > 500) { // 2 MB
        return 4;
    } else if (base > 250) { // 1 MB
        return 2;
    }

    return 1;
#endif
}

} // namespace

Pool pool() {
    const uint32_t base = (ESP.getFlashChipSize() / SPI_FLASH_SEC_SIZE) - 5;
    return Pool{ base, pool_size(base) };
}

bool read(uint32_t sector, uint8_t* data) {
    return ESP.flashRead(sector * SPI_FLASH_SEC_SIZE, reinterpret_cast<uint32_t*>(data), SPI_FLASH_SEC_SIZE);
}

bool erase(uint32_t sector) {
    return ESP.flashEraseSector(sector);
}

bool write(uint32_t sector, const uint8_t* data) {
    return ESP.flashWrite(sector * SPI_FLASH_SEC_SIZE,
        reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(data)), SPI_FLASH_SEC_SIZE);
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

void info(Print& out) {
    out.printf_P(PSTR("Flash: %u KiB\n"), ESP.getFlashChipSize() / 1024);
}

} // namespace flash
} // namespace storage
} // namespace espurna
