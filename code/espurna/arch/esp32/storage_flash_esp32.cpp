/*

SETTINGS STORAGE, the ESP32 flash (the storage itself is storage_eeprom.cpp, one for every chip)

The pool: the top 8 sectors of the data partition "spiffs" (esp32_4mb_safeboot.csv 0x3B0000, 256 KB; min_spiffs.csv of
a board not moved yet 0x3D0000, 128 KB). No file system uses it any more, nothing else is written there.

Before: one blob "eeprom" in the NVS namespace "eeprom" (the end of the 4 KiB buffer). Taken over once when the pool
has no valid copy yet, the blob is erased after the first write here: NVS is 20 KB with Matter's fabrics and the PHY
calibration in it. The ESP32 safeboot reads the pool and falls back to that blob (safeboot-app)

*/

#include "espurna.h"

#include "storage_eeprom.h"

#include <esp_partition.h>
#include <nvs.h>

#include <cstring>
#include <memory>

namespace espurna {
namespace storage {
namespace flash {
namespace {

constexpr uint8_t Sectors { 8 };

constexpr char LegacyNamespace[] = "eeprom";
constexpr char LegacyKey[] = "eeprom";

const esp_partition_t* partition() {
    static const esp_partition_t* const out =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    return out;
}

// offset of a pool sector in the partition
bool offset(uint32_t sector, size_t& out) {
    const auto* part = partition();
    if (!part) {
        return false;
    }

    const auto address = sector * SPI_FLASH_SEC_SIZE;
    if ((address < part->address) || ((address + SPI_FLASH_SEC_SIZE) > (part->address + part->size))) {
        return false;
    }

    out = address - part->address;
    return true;
}

} // namespace

Pool pool() {
    const auto* part = partition();
    if (!part || (part->size < (Sectors * SPI_FLASH_SEC_SIZE))) {
        return Pool{ 0, 0 };
    }

    return Pool{ ((part->address + part->size) / SPI_FLASH_SEC_SIZE) - 1, Sectors };
}

bool read(uint32_t sector, uint8_t* data) {
    size_t at;
    return offset(sector, at) && (esp_partition_read(partition(), at, data, SPI_FLASH_SEC_SIZE) == ESP_OK);
}

bool erase(uint32_t sector) {
    size_t at;
    return offset(sector, at) && (esp_partition_erase_range(partition(), at, SPI_FLASH_SEC_SIZE) == ESP_OK);
}

bool write(uint32_t sector, const uint8_t* data) {
    size_t at;
    return offset(sector, at) && (esp_partition_write(partition(), at, data, SPI_FLASH_SEC_SIZE) == ESP_OK);
}

bool keep(uint32_t) {
    return false;
}

// the blob is the end of the buffer (a longer one is not ours: its end)
bool legacy(uint8_t* data) {
    nvs_handle_t handle;
    if (nvs_open(LegacyNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    bool out { false };
    size_t size { 0 };
    if ((nvs_get_blob(handle, LegacyKey, nullptr, &size) == ESP_OK) && size) {
        std::unique_ptr<uint8_t[]> blob(new (std::nothrow) uint8_t[size]);
        if (blob && (nvs_get_blob(handle, LegacyKey, blob.get(), &size) == ESP_OK)) {
            const auto copy = std::min<size_t>(size, SPI_FLASH_SEC_SIZE);
            std::memcpy(&data[SPI_FLASH_SEC_SIZE - copy], &blob[size - copy], copy);
            out = true;
        }
    }

    nvs_close(handle);
    return out;
}

void done() {
    nvs_handle_t handle;
    if (nvs_open(LegacyNamespace, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, LegacyKey);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

void info(Print& out) {
    const auto* part = partition();
    if (part) {
        out.printf_P(PSTR("Partition \"%s\" 0x%06X, %u KiB\n"), part->label, part->address, part->size / 1024);
    }
}

} // namespace flash
} // namespace storage
} // namespace espurna
