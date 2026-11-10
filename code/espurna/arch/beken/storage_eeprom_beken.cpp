/*

EEPROM MODULE, BK72xx (LibreTiny) part (common part is in storage_eeprom.cpp and arch/rtos/storage_eeprom_rtos.cpp)

LibreTiny has no EEPROM library. The 4 KiB buffer is committed to the flash as a whole, rotating between several
slots of the 'userdata' partition. Every slot is two sectors, 4 KiB of data in the first one and the header in the
second one. Header is written last, so a slot with a valid header always has complete data. Latest valid slot is
loaded on boot.

Tuya layout keeps its own config at the end of 'userdata' (0x1ED000, 'tuya' partition, it has the stock GPIO
assignments), slots stay below it.

The buffer is the .bss of this object: platformio_beken.ini (custom_tcm_main_ram) keeps it in the main RAM by name

*/

#include "espurna.h"

#if defined(LIBRETINY)

#include "storage_eeprom.h"

#include <algorithm>
#include <cstring>

namespace {

constexpr uint32_t SlotsOffset { 0x1DB000 };
constexpr uint32_t SlotSize { 2 * FLASH_SECTOR_SIZE };
constexpr size_t Slots { 8 };

// crash record of rtcmem_beken.cpp at 0x1EB000, Tuya config at 0x1ED000
static_assert((SlotsOffset + (Slots * SlotSize)) <= 0x1EB000, "Slots overlap with the crash record");

constexpr uint32_t HeaderMagic { 0x45535042 }; // BPSE

struct Header {
    uint32_t magic;
    uint32_t sequence;
    uint32_t crc;
    uint32_t size;
};

uint8_t _eeprom_buffer[EepromSize];

// reads before eepromSetup() see an empty storage, same as an erased flash
struct BufferInit {
    BufferInit() {
        std::memset(_eeprom_buffer, 0xFF, EepromSize);
    }
} _eeprom_buffer_init;

size_t _eeprom_slot = 0;
uint32_t _eeprom_sequence = 0;

uint32_t _eepromSlotAddress(size_t slot) {
    return SlotsOffset + (slot * SlotSize);
}

uint32_t _eepromCrc(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
        }
    }

    return ~crc;
}

bool _eepromReadHeader(size_t slot, Header& header) {
    const auto address = _eepromSlotAddress(slot) + FLASH_SECTOR_SIZE;
    if (lt_flash_read(address, reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
        return false;
    }

    return (header.magic == HeaderMagic) && (header.size == EepromSize);
}

// Latest slot with a matching crc, older ones are fallbacks when the latest one is damaged
bool _eepromLoad() {
    struct Candidate {
        size_t slot;
        Header header;
    };

    Candidate candidates[Slots];
    size_t count = 0;

    for (size_t slot = 0; slot < Slots; ++slot) {
        Header header;
        if (_eepromReadHeader(slot, header)) {
            candidates[count++] = {slot, header};
        }
    }

    std::sort(candidates, candidates + count,
        [](const Candidate& lhs, const Candidate& rhs) {
            return lhs.header.sequence > rhs.header.sequence;
        });

    for (size_t index = 0; index < count; ++index) {
        const auto& candidate = candidates[index];
        if (lt_flash_read(_eepromSlotAddress(candidate.slot), _eeprom_buffer, EepromSize) != EepromSize) {
            continue;
        }

        if (_eepromCrc(_eeprom_buffer, EepromSize) != candidate.header.crc) {
            DEBUG_MSG_P(PSTR("[EEPROM] Slot #%u is damaged\n"), candidate.slot);
            continue;
        }

        _eeprom_slot = candidate.slot;
        _eeprom_sequence = candidate.header.sequence;
        return true;
    }

    std::memset(_eeprom_buffer, 0xFF, EepromSize);
    return false;
}

// Never overwrites the currently loaded slot, it stays valid until the new one is complete
bool _eepromWrite() {
    const auto slot = (_eeprom_slot + 1) % Slots;
    const auto address = _eepromSlotAddress(slot);

    if (!lt_flash_erase(address, SlotSize)) {
        return false;
    }

    if (lt_flash_write(address, _eeprom_buffer, EepromSize) != EepromSize) {
        return false;
    }

    Header header;
    header.magic = HeaderMagic;
    header.sequence = _eeprom_sequence + 1;
    header.crc = _eepromCrc(_eeprom_buffer, EepromSize);
    header.size = EepromSize;

    if (lt_flash_write(address + FLASH_SECTOR_SIZE, reinterpret_cast<const uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
        return false;
    }

    // read back, flash protection or a worn out sector silently drop writes
    Header check;
    if (!_eepromReadHeader(slot, check) || (check.sequence != header.sequence)) {
        return false;
    }

    _eeprom_slot = slot;
    _eeprom_sequence = header.sequence;

    return true;
}

} // namespace

namespace espurna {
namespace storage {
namespace chip {

uint8_t* buffer() {
    return _eeprom_buffer;
}

// no saved slot is an empty storage, the first write goes into slot #0
bool load() {
    if (_eepromLoad()) {
        DEBUG_MSG_P(PSTR("[EEPROM] Loaded slot #%u (#%u)\n"), _eeprom_slot, _eeprom_sequence);
    } else {
        _eeprom_slot = Slots - 1;
        _eeprom_sequence = 0;
        DEBUG_MSG_P(PSTR("[EEPROM] Warning: no saved settings\n"));
    }

    return true;
}

// buffer is only changed from the loop task, which is also the only one committing
bool save() {
    if (!_eepromWrite()) {
        DEBUG_MSG_P(PSTR("[EEPROM] Error: flash write failed, will retry\n"));
        return false;
    }

    DEBUG_MSG_P(PSTR("[EEPROM] Saved to slot #%u (#%u)\n"),
        _eeprom_slot, _eeprom_sequence);
    return true;
}

void info(Print& out, bool dirty) {
    out.printf_P(PSTR("Size: %u, slot #%u (0x%06X), sequence #%u, %s\n"),
        EepromSize, _eeprom_slot, _eepromSlotAddress(_eeprom_slot), _eeprom_sequence,
        dirty ? PSTR("changes pending") : PSTR("saved"));
}

String sectors() {
    String out;
    for (size_t slot = 0; slot < Slots; ++slot) {
        if (out.length()) {
            out += ", ";
        }
        out += String(_eepromSlotAddress(slot) / FLASH_SECTOR_SIZE, 10);
    }

    return out;
}

void dump(Print& out) {
    out.printf_P(PSTR("[EEPROM] Slot #%u, sequence #%u\n"), _eeprom_slot, _eeprom_sequence);
}

// first sector of the slots and how many they take
uint32_t base() {
    return SlotsOffset / FLASH_SECTOR_SIZE;
}

uint8_t pool() {
    return (Slots * SlotSize) / FLASH_SECTOR_SIZE;
}

} // namespace chip
} // namespace storage
} // namespace espurna

#endif // LIBRETINY
