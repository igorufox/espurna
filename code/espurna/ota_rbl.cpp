/*

OTA MODULE: the RBL package of a new head, ref. ota_rbl.h

*/

#include "espurna.h"

#include "ota_rbl.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace espurna {
namespace ota {
namespace rbl {

const char* header(const uint8_t* data, size_t len, espurna_rbl_t& out) {
    if (len < ESPURNA_RBL_SIZE) {
        return PSTR("ERROR: Upload too short for an RBL header");
    }

    std::memcpy(&out, data, ESPURNA_RBL_SIZE);
    return espurna_rbl_header_check(&out);
}

// --- Stream --------------------------------------------------------------------------------------------------------

void Stream::start(const Flash& flash, uint32_t at, uint32_t limit) {
    _flash = &flash;
    _base = at;
    _limit = limit;
    _written = 0;
    _erased = at & ~(Sector - 1);
    _staged = 0;
}

void Stream::erased_to(uint32_t end) {
    _erased = std::max(_erased, end);
}

bool Stream::flush() {
    if (!_staged) {
        return true;
    }

    const size_t size = (_staged + 3) & ~size_t{3};
    std::memset(&_stage[_staged], 0xFF, size - _staged);

    const uint32_t at = _base + _written;
    if ((at + size) > _limit) {
        return false;
    }

    while (_erased < (at + size)) {
        if (!_flash->erase(_erased)) {
            return false;
        }
        _erased += Sector;
    }

    if (!_flash->write(at, _stage, size)) {
        return false;
    }

    _written += _staged;
    _staged = 0;
    return true;
}

bool Stream::push(const uint8_t* data, size_t len) {
    while (len) {
        const size_t take = std::min(len, Stage - _staged);
        std::memcpy(&_stage[_staged], data, take);
        _staged += take;
        data += take;
        len -= take;
        if ((_staged == Stage) && !flush()) {
            return false;
        }
    }

    return true;
}

bool crc(const Flash& flash, uint32_t at, uint32_t size, uint32_t& out) {
    constexpr uint32_t Chunk { 1024 };
    std::unique_ptr<uint32_t[]> buffer(new (std::nothrow) uint32_t[Chunk / 4]);
    if (!buffer) {
        return false;
    }

    auto* bytes = reinterpret_cast<uint8_t*>(buffer.get());
    uint32_t value = 0;
    while (size) {
        const uint32_t take = std::min(size, Chunk);
        if (!flash.read(at, bytes, (take + 3) & ~uint32_t{3})) {
            return false;
        }
        value = espurna_rbl_crc32(value, bytes, take);
        at += take;
        size -= take;
    }

    out = value;
    return true;
}

// --- Writer --------------------------------------------------------------------------------------------------------

const char* Writer::begin(const Flash& flash, const espurna_rbl_t& header, const Place& place) {
    abort();

    // an older header goes first: nothing half-written is ever installed
    const uint32_t header_sector = place.header & ~(Sector - 1);
    if (!flash.erase(header_sector)) {
        return PSTR("ERROR: Flash erase failed");
    }

    _flash = &flash;
    _header = header;
    _place = place;
    _received = 0;
    _stream.start(flash, place.package, place.limit);
    if ((place.package & ~(Sector - 1)) == header_sector) {
        _stream.erased_to(header_sector + Sector);
    }

    _active = true;
    return nullptr;
}

const char* Writer::write(const uint8_t* data, size_t len) {
    if (!_active) {
        return PSTR("ERROR: No upload");
    }

    // the header was taken by begin(), the package follows it
    if (_received < ESPURNA_RBL_SIZE) {
        const size_t skip = std::min<size_t>(ESPURNA_RBL_SIZE - _received, len);
        _received += skip;
        data += skip;
        len -= skip;
    }

    if ((_received + len) > (ESPURNA_RBL_SIZE + _header.data_size)) {
        abort();
        return PSTR("ERROR: More data than the RBL header says");
    }

    if (len && !_stream.push(data, len)) {
        abort();
        return PSTR("ERROR: Flash write failed");
    }

    _received += len;
    return nullptr;
}

const char* Writer::end() {
    if (!_active || (_received != (ESPURNA_RBL_SIZE + _header.data_size))) {
        abort();
        return PSTR("ERROR: Incomplete image");
    }

    _active = false;

    if (!_stream.flush()) {
        return PSTR("ERROR: Flash write failed");
    }

    uint32_t value { 0 };
    if (!crc(*_flash, _place.package, _header.data_size, value) || (value != _header.data_crc)) {
        return PSTR("ERROR: The package in the flash does not match its CRC");
    }

    // the header last: from now on the bootloader installs it on the next boot
    if (!_flash->write(_place.header, reinterpret_cast<const uint8_t*>(&_header), ESPURNA_RBL_SIZE)) {
        return PSTR("ERROR: Flash write failed");
    }

    DEBUG_MSG_P(PSTR("[OTA] RBL %u bytes (package %u) at 0x%06X, the bootloader installs it\n"),
        _header.raw_size, _header.data_size, _place.package);
    return nullptr;
}

void Writer::abort() {
    _active = false;
}

// --- the confirmation of a new head --------------------------------------------------------------------------------

namespace {

constexpr uint32_t ConfirmAfterMs { 60000 };

const Flash* confirm_flash { nullptr };
uint32_t confirm_header { 0 };

void confirm_check() {
    if (!confirm_flash || (espurna::time::ms() < ConfirmAfterMs) || !wifiConnected()) {
        return;
    }

    const auto* flash = confirm_flash;
    confirm_flash = nullptr;
    if (flash->erase(confirm_header & ~(Sector - 1))) {
        DEBUG_MSG_P(PSTR("[OTA] The new head is confirmed, the old one is no longer kept\n"));
    }
}

} // namespace

void confirm(const Flash& flash, uint32_t header_at) {
    espurna_rbl_t header __attribute__((aligned(4)));
    if (!flash.read(header_at, reinterpret_cast<uint8_t*>(&header), sizeof(header))
        || (header.magic != ESPURNA_RBL_MAGIC))
    {
        return;
    }

    espurna_rbl_swap_t record __attribute__((aligned(4)));
    uint32_t done { 0xFFFFFFFF };
    flash.read(header_at + ESPURNA_RBL_SWAP_OFFSET, reinterpret_cast<uint8_t*>(&record), sizeof(record));
    flash.read(header_at + ESPURNA_RBL_DONE_OFFSET, reinterpret_cast<uint8_t*>(&done), sizeof(done));
    if ((record.magic != ESPURNA_RBL_SWAP_MAGIC) || done) {
        return;
    }

    confirm_flash = &flash;
    confirm_header = header_at;
    DEBUG_MSG_P(PSTR("[OTA] A new head: it confirms itself after 60 s with Wi-Fi, else the old one comes back\n"));
    espurnaRegisterLoop(confirm_check);
}

} // namespace rbl
} // namespace ota
} // namespace espurna
