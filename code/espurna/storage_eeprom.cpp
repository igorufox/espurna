/*

EEPROM MODULE

Common part of every chip, ref. storage_eeprom.h. Chip part is in arch/<chip>/storage_eeprom_<chip>.cpp

*/

#include "espurna.h"

#include "storage_eeprom.h"

#if WEB_SUPPORT
#include "ws.h"
#endif

#include <algorithm>
#include <cstring>

// settings layout on the flash of every chip is made for this size
static_assert(EepromSize == 4096, "");

namespace {

namespace chip = espurna::storage::chip;

bool _eeprom_ready = false;
bool _eeprom_dirty = false;          // a write happened since the last commit
bool _eeprom_commit_pending = false; // request from eepromCommit() handled in loop

// Bumped on every write. A write that lands while the buffer is being written must keep the storage dirty,
// otherwise it is silently lost after the reboot
uint32_t _eeprom_generation = 0;

// whatever the chip has to commit as well, e.g. the EEPROM_Rotate rotation flag
void _eepromCommitNeeded() {
    _eeprom_dirty = true;
    ++_eeprom_generation;
}

void _eepromMarkDirty() {
    _eepromCommitNeeded();
    chip::changed();
}

unsigned char _eepromRead(size_t address) {
    if (address >= EepromSize) return 0xFF;
    return chip::buffer()[address];
}

void _eepromWrite(size_t address, unsigned char value) {
    if (address >= EepromSize) return;
    if (chip::buffer()[address] != value) {
        chip::buffer()[address] = value;
        _eepromMarkDirty();
    }
}

void _eepromCommit() {
    if (!_eeprom_dirty || !(_eeprom_ready || chip::early())) {
        return;
    }

    const auto generation = _eeprom_generation;

    // on failure the storage stays dirty, the next commit request (or eepromLoop(), ref. chip::retry()) tries again
    if (chip::save()) {
        if (generation == _eeprom_generation) {
            _eeprom_dirty = false;
        } else {
            _eeprom_commit_pending = true;
            DEBUG_MSG_P(PSTR("[EEPROM] Settings changed during commit, will commit again.\n"));
        }
    }
}

void _eepromLoop() {
    static unsigned long _next_retry_ms = 0;
    if (_eeprom_commit_pending) {
        _eeprom_commit_pending = false;
        _eepromCommit();
        if (_eeprom_dirty && chip::retry()) {
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

#if TERMINAL_SUPPORT

STRING_VIEW_INLINE(EepromCommand, "EEPROM");

void _eepromCommand(::terminal::CommandContext&& ctx) {
    if (!_eeprom_ready) {
        terminalError(ctx, F("Storage is not available, settings are NOT saved"));
        return;
    }

    chip::info(ctx.output, _eeprom_dirty);
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
    chip::dump(ctx.output);
    terminalOK(ctx);
}

static constexpr ::terminal::Command EepromCommands[] PROGMEM {
    {EepromCommand, _eepromCommand},
    {EepromCommitCommand, _eepromCommandCommit},
    {EepromDumpCommand, _eepromCommandDump},
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
    return chip::buffer();
}

uint8_t* StorageEEPROM_Rotate::data() {
    return chip::buffer();
}

void StorageEEPROM_Rotate::setDirty() {
    _eepromMarkDirty();
}

void StorageEEPROM_Rotate::fill(uint8_t value) {
    auto* begin = chip::buffer() + EepromRotateOffset + EepromRotateReservedSize;
    auto* end = chip::buffer() + EepromSize;
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
    chip::dump(print);
}

uint32_t StorageEEPROM_Rotate::base() const {
    return chip::base();
}

uint8_t StorageEEPROM_Rotate::size() const {
    return chip::pool();
}

StorageEEPROM_Rotate& eepromInstance() {
    static StorageEEPROM_Rotate instance;
    return instance;
}

size_t eepromSpace() {
    return chip::space();
}

bool eepromReady() {
    return _eeprom_ready;
}

String eepromSectors() {
    return chip::sectors();
}

void eepromClear() {
    eepromInstance().fill(0xFF);
    _eepromCommit();
}

void eepromRotate(bool value) {
    if (chip::rotate(value)) {
        _eepromCommitNeeded();
        eepromCommit();
    }
}

void eepromBackup(uint32_t index) {
    chip::backup(index);
}

void eepromSetup() {
#if TERMINAL_SUPPORT
    _eepromCommandsSetup();
    chip::setup();
#endif

    if (!chip::load()) {
        // Everything keeps working in RAM, make sure it is visible that nothing is actually saved
        DEBUG_MSG_P(PSTR("[EEPROM] Error: Could not initialize the storage! Settings will not be saved.\n"));
#if WEB_SUPPORT
        wsRegister().onConnected([](JsonObject& root) {
            root[F("message")] = F("Settings storage is not available, changes will be lost on reboot!");
        });
#endif
        return;
    }

    espurnaRegisterLoop(_eepromLoop);
    _eeprom_ready = true;
}
