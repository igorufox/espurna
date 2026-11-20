#include "Update.h"

#include "core_chip.h"

namespace chip = espurna::core::chip;

UpdateClass Update;

void UpdateClass::fail(const char* error) {
    _error = error;
    if (_running) {
        chip::ota_abort();
        _running = false;
    }
}

bool UpdateClass::begin(size_t size) {
    if (_running) {
        fail("Already running");
        return false;
    }

    _error = nullptr;
    _finished = false;
    _progress = 0;
    _size = size;

    if (const char* error = chip::ota_begin(size)) {
        _error = error;
        return false;
    }

    _running = true;
    return true;
}

size_t UpdateClass::write(uint8_t* data, size_t len) {
    if (!_running || _error) {
        return 0;
    }

    if ((_size != UPDATE_SIZE_UNKNOWN) && (len > remaining())) {
        fail("Too much data");
        return 0;
    }

    if (const char* error = chip::ota_write(data, len)) {
        fail(error);
        return 0;
    }

    _progress += len;
    return len;
}

bool UpdateClass::end(bool evenIfRemaining) {
    if (!_running || _error) {
        return false;
    }

    if (!evenIfRemaining && (_size != UPDATE_SIZE_UNKNOWN) && (_progress != _size)) {
        fail("Not enough data");
        return false;
    }

    _running = false;
    if (const char* error = chip::ota_end()) {
        _error = error;
        return false;
    }

    _size = _progress;
    _finished = true;
    return true;
}

void UpdateClass::abort() {
    fail("Aborted");
}

void UpdateClass::printError(Print& out) {
    out.print(F("ERROR: "));
    out.println(errorString());
}
