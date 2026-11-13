/*

EEPROM MODULE

The one settings storage of every chip, ref. storage_eeprom.h: the RAM buffer, a pool of flash sectors in the format of
libs/settings_rotate.h (EEPROM_Rotate's), the newest valid copy loaded on the first use. The chip part is only its flash
and where the pool is: arch/<chip>/storage_flash_<chip>.cpp

*/

#include "espurna.h"

#include "storage_eeprom.h"
#include "libs/settings_rotate.h"

#if WEB_SUPPORT
#include "ws.h"
#endif

#include <algorithm>
#include <cstring>
#include <memory>

// settings layout on the flash of every chip is made for this size
static_assert(EepromSize == 4096, "");
static_assert(EepromSize == espurna::settings_rotate::SectorSize, "");
static_assert(EepromRotateOffset == espurna::settings_rotate::Offset, "");

namespace {

namespace flash = espurna::storage::flash;
namespace rotate = espurna::settings_rotate;

// --- the pool --------------------------------------------------------------------------------------------------

struct State {
    uint8_t* buffer { nullptr };
    flash::Pool pool { 0, 0 };
    size_t index { 0 };          // copy of the pool the settings were loaded from / last written to
    uint8_t counter { 0 };
    bool loaded { false };
    bool available { false };    // a pool to write to
    bool legacy { false };       // taken over from the chip's former storage, not written in this format yet
    bool rotation { true };
    bool boot_default { false }; // no valid copy at boot
    size_t commits { 0 };
    bool last_result { false };
};

State _eeprom;

uint32_t _eepromSector(size_t index) {
    return _eeprom.pool.base - index;
}

// on the first use, whichever static initializer, setup() or eepromSetup() comes first (heap: static DRAM of the ESP32
// is full on the larger images)
void _eepromLoad() {
    if (_eeprom.loaded) {
        return;
    }
    _eeprom.loaded = true;

    _eeprom.buffer = new uint8_t[EepromSize];
    _eeprom.pool = flash::pool();
    _eeprom.available = _eeprom.pool.size > 0;

    const auto found = _eeprom.available
        ? rotate::find(_eeprom.pool.size, _eeprom.buffer,
            [](size_t index, uint8_t* out) {
                return flash::read(_eepromSector(index), out);
            })
        : rotate::Found{ false, 0, 0 };

    if (found.valid) {
        _eeprom.index = found.index;
        _eeprom.counter = found.counter;
        return;
    }

    // the pool is empty: what the chip kept before, or empty settings. Written into the pool on the first commit
    std::memset(_eeprom.buffer, 0xFF, EepromSize);
    _eeprom.index = _eeprom.pool.size ? (_eeprom.pool.size - 1) : 0;
    _eeprom.counter = 0;
    _eeprom.legacy = _eeprom.available && flash::legacy(_eeprom.buffer);
    _eeprom.boot_default = !_eeprom.legacy;
    if (!_eeprom.legacy) {
        std::memset(_eeprom.buffer, 0xFF, EepromSize);
    }
}

uint8_t* _eepromBuffer() {
    _eepromLoad();
    return _eeprom.buffer;
}

// A copy into the index-th sector of the pool. `exact`: that one or none (the rotation is off, a backup); otherwise the
// first one from there that is not the current copy (it stays valid until the new one is complete) and not a sector
// the chip keeps
bool _eepromSave(size_t index, bool exact) {
    const auto count = _eeprom.pool.size;
    const bool current = !_eeprom.boot_default && !_eeprom.legacy;
    for (size_t attempt = 0; attempt < (exact ? 1 : count); ++attempt) {
        const auto candidate = (index + attempt) % count;
        if (flash::keep(_eepromSector(candidate))
            || (!exact && current && (count > 1) && (candidate == _eeprom.index)))
        {
            continue;
        }

        const auto counter = static_cast<uint8_t>(_eeprom.counter + 1);
        rotate::seal(_eeprom.buffer, counter);

        const auto sector = _eepromSector(candidate);
        if (!flash::erase(sector) || !flash::write(sector, _eeprom.buffer)) {
            return false;
        }

        _eeprom.index = candidate;
        _eeprom.counter = counter;
        if (_eeprom.legacy) {
            _eeprom.legacy = false;
            flash::done();
        }
        _eeprom.boot_default = false;
        return true;
    }

    return false;
}

bool _eepromWritePool() {
    if (!_eeprom.available) {
        return false;
    }

    const auto result = _eeprom.rotation
        ? _eepromSave((_eeprom.index + 1) % _eeprom.pool.size, false)
        : _eepromSave(0, true);
    _eeprom.last_result = result;
    if (result) {
        ++_eeprom.commits;
    }

    return result;
}

// --- the buffer and its commits --------------------------------------------------------------------------------

bool _eeprom_ready = false;
bool _eeprom_dirty = false;          // a write happened since the last commit
bool _eeprom_commit_pending = false; // request from eepromCommit() handled in loop

// Bumped on every write. A write that lands while the buffer is being written must keep the storage dirty,
// otherwise it is silently lost after the reboot
uint32_t _eeprom_generation = 0;

void _eepromMarkDirty() {
    _eeprom_dirty = true;
    ++_eeprom_generation;
}

unsigned char _eepromRead(size_t address) {
    if (address >= EepromSize) return 0xFF;
    return _eepromBuffer()[address];
}

void _eepromWrite(size_t address, unsigned char value) {
    if (address >= EepromSize) return;
    auto* buffer = _eepromBuffer();
    if (buffer[address] != value) {
        buffer[address] = value;
        _eepromMarkDirty();
    }
}

// works before eepromSetup() as well (the buffer is loaded on the first use)
void _eepromCommit() {
    if (!_eeprom_dirty) {
        return;
    }

    _eepromLoad();
    const auto generation = _eeprom_generation;

    // on failure the storage stays dirty, the next commit request or the retry of _eepromLoop() tries again
    if (_eepromWritePool()) {
        if (generation == _eeprom_generation) {
            _eeprom_dirty = false;
        } else {
            _eeprom_commit_pending = true;
            DEBUG_MSG_P(PSTR("[EEPROM] Settings changed during commit, will commit again.\n"));
        }
    } else {
        DEBUG_MSG_P(PSTR("[EEPROM] Error: flash write failed, will retry\n"));
    }
}

void _eepromLoop() {
    static unsigned long _next_retry_ms = 0;
    if (_eeprom_commit_pending) {
        _eeprom_commit_pending = false;
        _eepromCommit();
        if (_eeprom_dirty) {
            // Commit just failed - schedule a backoff retry
            _next_retry_ms = millis() + 5000;
        }
        return;
    }

    // Background retry: if a prior commit failed and nothing triggered another save in a while, attempt it again
    // every 5 seconds
    if (_eeprom_dirty && _next_retry_ms && (long)(millis() - _next_retry_ms) >= 0) {
        _next_retry_ms = millis() + 5000;
        _eepromCommit();
        if (!_eeprom_dirty) {
            _next_retry_ms = 0;
        }
    }
}

String _eepromSectors() {
    String out;
    for (size_t index = 0; index < _eeprom.pool.size; ++index) {
        if (index) {
            out += STRING_VIEW(", ").toString();
        }

        const bool current = (index == _eeprom.index) && !_eeprom.boot_default && !_eeprom.legacy;
        if (current) {
            out += '(';
        }
        out += String(_eepromSector(index));
        if (current) {
            out += ')';
        }
    }

    return out;
}

void _eepromDump(Print& out, const uint8_t* data) {
    char ascii[17];
    ascii[16] = '\0';
    for (size_t address = 0; address < EepromSize; address += 16) {
        out.printf_P(PSTR("0x%04X: "), address);
        for (size_t index = 0; index < 16; ++index) {
            const auto value = data[address + index];
            out.printf_P(PSTR("%02X "), value);
            ascii[index] = ((value > 31) && (value < 127)) ? static_cast<char>(value) : '.';
        }
        out.println(ascii);
        yield();
    }
}

#if TERMINAL_SUPPORT

STRING_VIEW_INLINE(EepromCommand, "EEPROM");

void _eepromCommand(::terminal::CommandContext&& ctx) {
    if (!_eeprom_ready) {
        terminalError(ctx, F("Storage is not available, settings are NOT saved"));
        return;
    }

    ctx.output.printf_P(PSTR("Sector%s: %s, %s\n"),
        (_eeprom.pool.size > 1) ? "s" : "", _eepromSectors().c_str(),
        _eeprom_dirty ? PSTR("changes pending") : PSTR("saved"));
    ctx.output.printf_P(PSTR("Commits done: %u, last: %s, boot: %s\n"),
        _eeprom.commits,
        _eeprom.commits ? (_eeprom.last_result ? PSTR("OK") : PSTR("ERROR")) : PSTR("none"),
        _eeprom.legacy ? PSTR("taken over, not written yet")
            : _eeprom.boot_default ? PSTR("empty") : PSTR("loaded"));
    flash::info(ctx.output);
    terminalOK(ctx);
}

STRING_VIEW_INLINE(EepromCommitCommand, "EEPROM.COMMIT");

void _eepromCommandCommit(::terminal::CommandContext&& ctx) {
    _eepromCommit();
    if (_eeprom_dirty) {
        terminalError(ctx, F("Commit failed"));
        return;
    }

    terminalOK(ctx);
}

STRING_VIEW_INLINE(EepromDumpCommand, "EEPROM.DUMP");

void _eepromCommandDump(::terminal::CommandContext&& ctx) {
    _eepromDump(ctx.output, _eepromBuffer());
    terminalOK(ctx);
}

// FLASH.DUMP <sector>: a sector of the pool as it is on the flash
STRING_VIEW_INLINE(FlashDumpCommand, "FLASH.DUMP");

void _eepromCommandFlashDump(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() < 2) {
        terminalError(ctx, _eepromSectors());
        return;
    }

    const auto sector = espurna::settings::internal::convert<uint32_t>(ctx.argv[1]);
    if ((sector > _eeprom.pool.base) || ((_eeprom.pool.base - sector) >= _eeprom.pool.size)) {
        terminalError(ctx, F("Not a sector of the settings pool"));
        return;
    }

    std::unique_ptr<uint8_t[]> data(new (std::nothrow) uint8_t[EepromSize]);
    if (!data || !flash::read(sector, data.get())) {
        terminalError(ctx, F("Cannot read"));
        return;
    }

    _eepromDump(ctx.output, data.get());
    terminalOK(ctx);
}

static constexpr ::terminal::Command EepromCommands[] PROGMEM {
    {EepromCommand, _eepromCommand},
    {EepromCommitCommand, _eepromCommandCommit},
    {EepromDumpCommand, _eepromCommandDump},
    {FlashDumpCommand, _eepromCommandFlashDump},
};

void _eepromCommandsSetup() {
    espurna::terminal::add(EepromCommands);
}
#endif

} // namespace

void eepromCommit() {
    _eeprom_commit_pending = true;
}

void eepromForceCommit() {
    _eepromCommit();
}

void eepromForceCommit(StorageEEPROM_Rotate&) {
    _eepromCommit();
}

// Settings / embedis write straight through this pointer (memmove_P) and then mark the storage dirty
const uint8_t* StorageEEPROM_Rotate::data() const {
    return _eepromBuffer();
}

uint8_t* StorageEEPROM_Rotate::data() {
    return _eepromBuffer();
}

void StorageEEPROM_Rotate::setDirty() {
    _eepromMarkDirty();
}

void StorageEEPROM_Rotate::fill(uint8_t value) {
    auto* begin = _eepromBuffer() + EepromReservedSize;
    auto* end = _eepromBuffer() + EepromSize;
    std::fill(begin, end, value);
    _eepromMarkDirty();
}

bool StorageEEPROM_Rotate::commit() {
    _eepromCommit();
    return !_eeprom_dirty;
}

uint8_t StorageEEPROM_Rotate::read(size_t address) {
    return _eepromRead(address);
}

void StorageEEPROM_Rotate::write(size_t address, uint8_t value) {
    _eepromWrite(address, value);
}

void StorageEEPROM_Rotate::dump(Print& print) const {
    _eepromDump(print, _eepromBuffer());
}

uint32_t StorageEEPROM_Rotate::base() const {
    _eepromLoad();
    return _eeprom.pool.base;
}

uint8_t StorageEEPROM_Rotate::size() const {
    _eepromLoad();
    return _eeprom.pool.size;
}

StorageEEPROM_Rotate& eepromInstance() {
    static StorageEEPROM_Rotate instance;
    return instance;
}

size_t eepromSpace() {
    _eepromLoad();
    return _eeprom.pool.size * EepromSize;
}

bool eepromReady() {
    return _eeprom_ready;
}

String eepromSectors() {
    _eepromLoad();
    return _eepromSectors();
}

void eepromClear() {
    eepromInstance().fill(0xFF);
    _eepromCommit();
}

// OTA: while it is off every commit goes into the base sector (an image written into the free space may reach the
// lower sectors of the pool)
void eepromRotate(bool value) {
    _eepromLoad();
    if (_eeprom.pool.size < 2) {
        return;
    }

    DEBUG_MSG_P(PSTR("[EEPROM] %s rotation\n"), value ? PSTR("Enabling") : PSTR("Disabling"));
    _eeprom.rotation = value;
    _eepromMarkDirty();
    eepromCommit();
}

// a copy into the given sector of the pool right away (0 - the base sector)
void eepromBackup(uint32_t sector) {
    _eepromLoad();
    if (!_eeprom.available) {
        return;
    }

    if (!sector) {
        sector = _eeprom.pool.base;
    }

    if ((sector > _eeprom.pool.base) || ((_eeprom.pool.base - sector) >= _eeprom.pool.size)) {
        return;
    }

    const auto index = _eeprom.pool.base - sector;
    if ((index == _eeprom.index) && !_eeprom.boot_default && !_eeprom.legacy) {
        return;
    }

    _eeprom.last_result = _eepromSave(index, true);
    if (_eeprom.last_result) {
        ++_eeprom.commits;
    }
}

void eepromSetup() {
#if TERMINAL_SUPPORT
    _eepromCommandsSetup();
#endif

    _eepromLoad();
    if (!_eeprom.available) {
        // Everything keeps working in RAM, make sure it is visible that nothing is actually saved
        DEBUG_MSG_P(PSTR("[EEPROM] Error: Could not initialize the storage! Settings will not be saved.\n"));
#if WEB_SUPPORT
        wsRegister().onConnected([](JsonObject& root) {
            root[F("message")] = F("Settings storage is not available, changes will be lost on reboot!");
        });
#endif
        return;
    }

    if (_eeprom.legacy) {
        DEBUG_MSG_P(PSTR("[EEPROM] Settings taken over from the former storage, written on the first commit\n"));
        _eepromMarkDirty();
        eepromCommit();
    } else if (_eeprom.boot_default) {
        DEBUG_MSG_P(PSTR("[EEPROM] Warning: no saved settings\n"));
    } else {
        DEBUG_MSG_P(PSTR("[EEPROM] Sector %u (#%u)\n"), _eepromSector(_eeprom.index), _eeprom.counter);
    }

    espurnaRegisterLoop(_eepromLoop);
    _eeprom_ready = true;
}
