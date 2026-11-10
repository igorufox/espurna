/*

EEPROM MODULE, ESP32 part (common part is in storage_eeprom.cpp and arch/rtos/storage_eeprom_rtos.cpp)

Settings are kept in the NVS blob "eeprom" of the namespace "eeprom", like the Arduino-ESP32 EEPROM library keeps them.
Only the used end of the buffer is written, not all 4 KiB: NVS needs room for a new copy of a blob before the old one
goes, and with the Matter fabrics (CHIP_KVS) and the PHY calibration in the same 20 KiB partition a 4 KiB blob (131
entries) could no longer be rewritten - every commit failed and the settings went back after a reboot.

The buffer is rebuilt as 0xFF and the blob at its end, so a 4096 byte blob of the library reads the same. The ESPurna
safeboot reads the blob the same way (any length, from its end)

*/

#include "espurna.h"

#if defined(ARDUINO_ARCH_ESP32)

#include "storage_eeprom.h"

#include <nvs.h>

#include <cstring>
#include <memory>

namespace {

constexpr char Namespace[] = "eeprom";
constexpr char Key[] = "eeprom";

// Heap, not .bss: static DRAM of the ESP32 is full on the larger images (NSPanel). An empty store until
// eepromSetup() loads it (settings may be read before)
uint8_t* _eepromAllocate() {
    auto* out = new uint8_t[EepromSize];
    std::memset(out, 0xFF, EepromSize);
    return out;
}

nvs_handle_t _eeprom_handle { 0 };
esp_err_t _eeprom_last_error = ESP_OK;

// Bytes from the first one that is not 0xFF to the end. Whatever 0xFF bytes are dropped in front are 0xFF again
// when the buffer is rebuilt, so nothing is lost even when a value starts with one
size_t _eepromUsed() {
    const auto* buffer = espurna::storage::chip::buffer();

    size_t start = 0;
    while ((start < EepromSize) && (buffer[start] == 0xFF)) {
        ++start;
    }

    // keep at least the end marker of an empty store
    return std::max<size_t>(EepromSize - start, 2);
}

bool _eepromLoad(uint8_t* buffer) {
    if (nvs_open(Namespace, NVS_READWRITE, &_eeprom_handle) != ESP_OK) {
        return false;
    }

    size_t size { 0 };
    const auto err = nvs_get_blob(_eeprom_handle, Key, nullptr, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }

    if ((err != ESP_OK) || !size) {
        return err == ESP_OK;
    }

    // a longer blob is not ours, read its end
    if (size > EepromSize) {
        std::unique_ptr<uint8_t[]> blob(new (std::nothrow) uint8_t[size]);
        if (!blob || (nvs_get_blob(_eeprom_handle, Key, blob.get(), &size) != ESP_OK)) {
            return false;
        }

        std::memcpy(buffer, &blob[size - EepromSize], EepromSize);
        return true;
    }

    return nvs_get_blob(_eeprom_handle, Key, &buffer[EepromSize - size], &size) == ESP_OK;
}

} // namespace

namespace espurna {
namespace storage {
namespace chip {

// on the first use, whichever static initializer or setup() comes first
uint8_t* buffer() {
    static uint8_t* const out = _eepromAllocate();
    return out;
}

bool load() {
    if (!_eepromLoad(buffer())) {
        std::memset(buffer(), 0xFF, EepromSize);
        return false;
    }

    const auto used = _eepromUsed();
    if (used <= 2) {
        DEBUG_MSG_P(PSTR("[EEPROM] Warning: Memory is empty (all 0xFF).\n"));
    } else {
        DEBUG_MSG_P(PSTR("[EEPROM] Ready. %u of %u bytes used\n"), used, EepromSize);
    }

    return true;
}

bool save() {
    const auto used = _eepromUsed();

    DEBUG_MSG_P(PSTR("[EEPROM] Committing %u bytes to NVS...\n"), used);
    auto err = nvs_set_blob(_eeprom_handle, Key, &buffer()[EepromSize - used], used);
    if (err == ESP_OK) {
        err = nvs_commit(_eeprom_handle);
    }

    _eeprom_last_error = err;
    if (err != ESP_OK) {
        DEBUG_MSG_P(PSTR("[EEPROM] Error: NVS commit failed (%s), will retry.\n"), esp_err_to_name(err));
        return false;
    }

    DEBUG_MSG_P(PSTR("[EEPROM] Settings saved successfully.\n"));
    return true;
}

void info(Print& out, bool dirty) {
    out.printf_P(PSTR("ESP32 EEPROM on NVS. Size: %u, used: %u, %s, last commit: %s\n"),
        EepromSize, _eepromUsed(),
        dirty ? PSTR("changes pending") : PSTR("saved"),
        esp_err_to_name(_eeprom_last_error));
}

String sectors() {
    return String("");
}

void dump(Print& out) {
    out.println("[EEPROM] ESP32 Dump");
}

// NVS, not in flash sectors of its own
uint32_t base() {
    return 0;
}

uint8_t pool() {
    return 0;
}

} // namespace chip
} // namespace storage
} // namespace espurna

#endif // ARDUINO_ARCH_ESP32
