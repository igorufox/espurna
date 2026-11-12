/*

EEPROM MODULE, ESP8266 part (common part is in storage_eeprom.cpp)

Settings sector is EEPROM_Rotate's: the base sector right under the SDK area and the pool of copies under it, sized
by the flash (the copies of a 1 MB chip are in 251 and 250). Same sectors and the same CRC + counter bytes as always.
EEPROM_Rotate loads the latest copy on the first use, before eepromSetup(), and only writes what it knows is changed.

*/

#include "../../espurna.h"

#include "../../storage_eeprom.h"

#include <EEPROM_Rotate.h>

namespace {

size_t _eeprom_commit_count = 0;
bool _eeprom_last_commit_result = false;

bool _eepromCommitResult(bool value) {
    // note that even an no-op commit is a 'success'
    if (value) {
        _eeprom_commit_count += 1;
    }

    _eeprom_last_commit_result = value;
    return value;
}

class Rotate : public EEPROM_Rotate {
public:
    Rotate() :
        EEPROM_Rotate()
    {
#ifdef EEPROM_ROTATE_SECTORS
        static_assert(EEPROM_ROTATE_SECTORS > 0, "");
        _pool_size = EEPROM_ROTATE_SECTORS;
#else
        // ESP8266 environment uses fixed addresses for globally accessible `FS` & `EEPROM`.
        // By default, last() is `(FLASH_SIZE / SECTOR_SIZE) - 5` aka base EEPROM address.
        //
        // EEPROM_Rotate also tries to check whether the `_FS_end` allows for any sectors
        // between it and `_EEPROM_start`. But, this is generally never true.
        const auto last = this->last();

        // Thus, override original behaviour in regards to size limits and auto-size detection
        // `last()` returns the number of available sectors MINUS reserved by the SDK
        // Prefer to ignore the embedded numbers and use generic `SIZE * 2`.
        if (last > 4000) { // 16Mb
            _pool_size = 32;
        } else if (last > 2000) { // 8Mb
            _pool_size = 16;
        } else if (last > 1000) { // 4Mb
            _pool_size = 8;
        } else if (last > 500) { // 2Mb
            _pool_size = 4;
        } else if (last > 250) { // 1Mb
            _pool_size = 2;
        }
#endif

        _offset = EepromRotateOffset;
        EEPROM_Rotate::begin(EepromSize);

        // With all other things equal, make sure this sector could be written to later
        if (!_checkCRC()) {
            fill_settings(0xff);
            writeReservedData();
            _eepromCommitResult(EEPROMClass::commit());
            _boot_default = true;
        }
    }

    // not getDataPtr(), that one marks the data as changed
    uint8_t* data() {
        return _data;
    }

    void changed() {
        _dirty = true;
    }

    // exception for very small flash / forced size(VALUE)
    bool canRotate() const {
        return _pool_size > 1;
    }

    // Nth sector of the pool
    uint32_t sector(uint8_t index) {
        return _getSector(index);
    }

    bool boot_default() const {
        return _boot_default;
    }

private:
    void fill_settings(uint8_t value) {
        auto* begin = &_data[0] + _offset + EepromRotateReservedSize;
        auto* end = &_data[0] + _size;
        if (_size && (begin < end)) {
            _dirty = true;
            std::fill(begin, end, value);
        }
    }

    // ensure CRC + VALUE for the *current sector* exist on the flash
    void writeReservedData() {
        uint16_t crc = _calculateCRC(); // TODO use poly version vs. sum?
        write(_offset + EEPROM_ROTATE_CRC_OFFSET, (crc >> 8) & 0xFF);
        write(_offset + EEPROM_ROTATE_CRC_OFFSET + 1, crc & 0xFF);
        write(_offset + EEPROM_ROTATE_COUNTER_OFFSET, _sector_value);
    }

    bool _boot_default { false };
};

Rotate& backend() {
    static Rotate instance;
    return instance;
}

String _eepromAvailableSectors() {
    auto& instance = backend();
    const auto current_sector = instance.current();

    String out;
    for (uint8_t i = 0; i < instance.size(); ++i) {
        if (i > 0) {
            out += STRING_VIEW(", ").toString();
        }

        const auto sector = instance.sector(i);
        if (sector == current_sector) {
            out += '(';
        }

        out += String(sector);
        if (sector == current_sector) {
            out += ')';
        }
    }

    return out;
}

#if TERMINAL_SUPPORT

STRING_VIEW_INLINE(FlashDump, "FLASH.DUMP");

void _flashCommandDump(::terminal::CommandContext&& ctx) {
    static const uint32_t Sectors = ESP.getFlashChipSize() / SPI_FLASH_SEC_SIZE;
    if (ctx.argv.size() < 2) {
        terminalError(ctx, _eepromAvailableSectors());
        return;
    }
    uint32_t sector = espurna::settings::internal::convert<uint32>(ctx.argv[1]);
    if (sector >= Sectors) {
        terminalError(ctx, STRING_VIEW("Sector out of range").toString());
        return;
    }

    // XXX only Print interface is used, assume this is safe
    backend().dump(static_cast<Stream&>(ctx.output), sector);
    terminalOK(ctx);
}

static constexpr ::terminal::Command FlashCommands[] PROGMEM {
    {FlashDump, _flashCommandDump},
};

#endif

} // namespace

namespace espurna {
namespace storage {
namespace chip {

uint8_t* buffer() {
    return backend().data();
}

// already there, ref. Rotate()
bool load() {
    backend();
    return true;
}

bool early() {
    return true;
}

void changed() {
    backend().changed();
}

bool save() {
    return _eepromCommitResult(backend().commit());
}

bool retry() {
    return false;
}

void setup() {
#if TERMINAL_SUPPORT
    espurna::terminal::add(FlashCommands);
#endif
}

void info(Print& out, bool) {
    auto& instance = backend();
    out.printf_P(PSTR("Sector%s: %s\n"),
        instance.canRotate() ? "s" : "",
        _eepromAvailableSectors().c_str());

    if (_eeprom_commit_count > 0) {
        out.printf_P(PSTR("Commits done: %lu, last: %s, boot reset: %s\n"),
            _eeprom_commit_count,
            _eeprom_last_commit_result
                ? PSTR("OK")
                : PSTR("ERROR"),
            instance.boot_default()
                ? PSTR("YES")
                : PSTR("NO"));
    }
}

String sectors() {
    return _eepromAvailableSectors();
}

size_t space() {
    return backend().size() * size_t{ SPI_FLASH_SEC_SIZE };
}

// XXX only Print interface is used, assume this is safe
void dump(Print& out) {
    backend().dump(static_cast<Stream&>(out));
}

uint32_t base() {
    return backend().base();
}

uint8_t pool() {
    return backend().size();
}

// Only matters when the instance actually allows rotation.
// Because .rotate(false) marks EEPROM as dirty, this is equivalent to the .backup(0)
bool rotate(bool value) {
    auto& instance = backend();
    if (instance.canRotate()) {
        DEBUG_MSG_P(PSTR("[EEPROM] %s EEPROM rotation\n"),
            value ? PSTR("Enabling") : PSTR("Disabling"));
        instance.rotate(value);
        return true;
    }

    return false;
}

void backup(uint32_t index) {
    ::backend().backup(index);
}

} // namespace chip
} // namespace storage
} // namespace espurna

