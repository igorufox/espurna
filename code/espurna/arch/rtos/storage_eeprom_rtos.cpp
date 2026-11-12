/*

EEPROM MODULE, what both FreeRTOS chips (ESP32, BK72xx with LibreTiny) do the same way

Common part is in storage_eeprom.cpp, the rest of the chip part in arch/<chip>/storage_eeprom_<chip>.cpp

*/

#include "espurna.h"

#include "storage_eeprom.h"

namespace espurna {
namespace storage {
namespace chip {

// loaded by eepromSetup(), nothing is committed before that
bool early() {
    return false;
}

// save() writes the whole buffer whenever it is called, nothing to track
void changed() {
}

bool retry() {
    return true;
}

void setup() {
}

size_t space() {
    return EepromSize;
}

// no rotation to turn off for the OTA, no backup copy to make
bool rotate(bool) {
    return false;
}

void backup(uint32_t) {
}

} // namespace chip
} // namespace storage
} // namespace espurna

