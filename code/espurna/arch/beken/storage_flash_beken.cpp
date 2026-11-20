/*

SETTINGS STORAGE, the BK72xx flash (beken_api.h flash_*; the storage itself is storage_eeprom.cpp, one for every chip)

The pool: the 16 sectors of 'userdata' from 0x1DB000 up to the crash record of rtcmem_beken.cpp (0x1EB000; the Tuya
config with the stock GPIO assignments is above it, 0x1ED000), the base sector the top one, 0x1EA000.

Before: 8 slots of two sectors in the same place (the 4 KiB buffer in the first one, a header with a CRC-32 and a
sequence in the second one, magic BPSE). Taken over once when the pool has no valid copy yet; the two sectors of that
slot are kept until the settings are written in this format. The sector of an RF calibration block the SDK does not
have yet (system_beken.cpp) is never erased

*/

#include "beken_api.h"
#include "../../espurna.h"

#include "../../storage_eeprom.h"
#include "../../libs/crc32_ieee.h"

#include <algorithm>
#include <cstring>

// system_beken.cpp: a vendor RF calibration block in the pool that was not copied yet, 0 - none
uint32_t bekenCalibrationKeep();

namespace espurna {
namespace storage {
namespace flash {
namespace {

constexpr uint32_t RegionOffset { 0x1DB000 };
constexpr uint32_t RegionEnd { 0x1EB000 };
constexpr uint8_t Sectors { (RegionEnd - RegionOffset) / FLASH_SECTOR_SIZE };

// crash record of rtcmem_beken.cpp at 0x1EB000, Tuya config at 0x1ED000
static_assert(RegionEnd <= 0x1EB000, "The pool overlaps with the crash record");

namespace legacy_slots {

constexpr uint32_t SlotSize { 2 * FLASH_SECTOR_SIZE };
constexpr size_t Slots { 8 };
constexpr uint32_t HeaderMagic { 0x45535042 }; // BPSE

struct Header {
    uint32_t magic;
    uint32_t sequence;
    uint32_t crc;
    uint32_t size;
};

// the slot the settings were taken over from, 0 - none (any more)
uint32_t taken { 0 };

uint32_t address(size_t slot) {
    return RegionOffset + (slot * SlotSize);
}

bool header(size_t slot, Header& out) {
    if (::espurna::beken::flash_read(address(slot) + FLASH_SECTOR_SIZE, reinterpret_cast<uint8_t*>(&out), sizeof(out)) != sizeof(out)) {
        return false;
    }

    return (out.magic == HeaderMagic) && (out.size == SPI_FLASH_SEC_SIZE);
}

// the newest slot with a matching CRC
bool load(uint8_t* data) {
    struct Candidate {
        size_t slot;
        uint32_t sequence;
        uint32_t crc;
    };

    Candidate candidates[Slots];
    size_t count { 0 };
    for (size_t slot = 0; slot < Slots; ++slot) {
        Header value;
        if (header(slot, value)) {
            candidates[count++] = Candidate{ slot, value.sequence, value.crc };
        }
    }

    std::sort(candidates, candidates + count,
        [](const Candidate& lhs, const Candidate& rhs) {
            return lhs.sequence > rhs.sequence;
        });

    for (size_t index = 0; index < count; ++index) {
        const auto& candidate = candidates[index];
        if (::espurna::beken::flash_read(address(candidate.slot), data, SPI_FLASH_SEC_SIZE) != SPI_FLASH_SEC_SIZE) {
            continue;
        }

        if (::espurna::crc32_ieee::crc32Update(0, data, SPI_FLASH_SEC_SIZE) != candidate.crc) {
            continue;
        }

        taken = address(candidate.slot);
        return true;
    }

    return false;
}

} // namespace legacy_slots

uint32_t address(uint32_t sector) {
    return sector * FLASH_SECTOR_SIZE;
}

bool inside(uint32_t sector) {
    const auto at = address(sector);
    return (at >= RegionOffset) && ((at + FLASH_SECTOR_SIZE) <= RegionEnd);
}

} // namespace

Pool pool() {
    return Pool{ (RegionEnd / FLASH_SECTOR_SIZE) - 1, Sectors };
}

bool read(uint32_t sector, uint8_t* data) {
    return inside(sector) && (::espurna::beken::flash_read(address(sector), data, FLASH_SECTOR_SIZE) == FLASH_SECTOR_SIZE);
}

bool erase(uint32_t sector) {
    return inside(sector) && ::espurna::beken::flash_erase(address(sector), FLASH_SECTOR_SIZE);
}

// read back: flash protection or a worn out sector silently drop writes
bool write(uint32_t sector, const uint8_t* data) {
    if (!inside(sector) || (::espurna::beken::flash_write(address(sector), data, FLASH_SECTOR_SIZE) != FLASH_SECTOR_SIZE)) {
        return false;
    }

    uint8_t check[64];
    for (uint32_t at = 0; at < FLASH_SECTOR_SIZE; at += sizeof(check)) {
        if ((::espurna::beken::flash_read(address(sector) + at, check, sizeof(check)) != sizeof(check))
            || (std::memcmp(check, &data[at], sizeof(check)) != 0))
        {
            return false;
        }
    }

    return true;
}

bool keep(uint32_t sector) {
    const auto at = address(sector);

    const auto calibration = bekenCalibrationKeep();
    if (calibration && (calibration < (at + FLASH_SECTOR_SIZE)) && (at < (calibration + FLASH_SECTOR_SIZE))) {
        return true;
    }

    const auto taken = legacy_slots::taken;
    return taken && (at >= taken) && (at < (taken + legacy_slots::SlotSize));
}

bool legacy(uint8_t* data) {
    return legacy_slots::load(data);
}

void done() {
    legacy_slots::taken = 0;
}

bool legacyErasedWhenDone() {
    return false;
}

void info(::espurna::Output& out) {
    out.printf_P(PSTR("Region 0x%06X..0x%06X\n"), RegionOffset, RegionEnd);
}

} // namespace flash
} // namespace storage
} // namespace espurna
