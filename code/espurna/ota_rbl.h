/*

OTA MODULE: the RBL package of a new head (libs/rbl.h), the same on every chip (ESP8266, ESP32, Beken). The
header comes first in the upload and is written LAST, after the package is in the flash and its CRC32 read back matches:
until then the bootloader finds nothing to install, an interrupted upload installs nothing.

The chip gives only what its flash and its bootloader force:
- Flash: erase a sector, write, read (the writes come in 4-byte multiples from a 4-byte aligned buffer)
- Place: where the header goes, where the package goes (right after the header, or anywhere else), where it must end
- its own checks of the header (the name, the kind of the package, where it fits) before the place is taken

*/

#pragma once

#include <cstddef>
#include <cstdint>

#include "libs/rbl.h"

namespace espurna {
namespace ota {
namespace rbl {

constexpr uint32_t Sector { 0x1000 };

struct Flash {
    bool (*erase)(uint32_t at);                                 // the sector at `at` (aligned)
    bool (*write)(uint32_t at, const uint8_t* data, size_t size);
    bool (*read)(uint32_t at, uint8_t* data, size_t size);
};

struct Place {
    uint32_t header;        // where the 96 bytes of the header go
    uint32_t package;       // where the package goes
    uint32_t limit;         // the package and its padding end at or before this
};

// The upload's header: at least ESPURNA_RBL_SIZE bytes of `data`, copied to `out` and checked (libs/rbl.h). nullptr -
// fine, else what is wrong
const char* header(const uint8_t* data, size_t len, espurna_rbl_t& out);

// A stream into the flash in aligned blocks, erasing the sectors ahead of it (also the bootloader route of ESP8266)
class Stream {
public:
    static constexpr size_t Stage { 512 };

    void start(const Flash& flash, uint32_t at, uint32_t limit);
    bool push(const uint8_t* data, size_t len);
    bool flush();

    // the sector at `at` is erased already (the header's, when the package shares it)
    void erased_to(uint32_t end);

    uint32_t written() const {
        return _written;
    }

private:
    const Flash* _flash { nullptr };
    uint32_t _base { 0 };
    uint32_t _limit { 0 };
    uint32_t _written { 0 };
    uint32_t _erased { 0 };     // the flash is erased from the sector of _base up to here
    uint8_t _stage[Stage] __attribute__((aligned(4))) {};
    size_t _staged { 0 };
};

// The CRC32 of what is in the flash
bool crc(const Flash& flash, uint32_t at, uint32_t size, uint32_t& out);

// One RBL upload: begin() with the checked header and its place, write() with the bytes of the upload as they come
// (the header again first: skipped), end() checks and writes the header
class Writer {
public:
    const char* begin(const Flash& flash, const espurna_rbl_t& header, const Place& place);
    const char* write(const uint8_t* data, size_t len);
    const char* end();
    void abort();

    bool active() const {
        return _active;
    }

private:
    const Flash* _flash { nullptr };
    espurna_rbl_t _header __attribute__((aligned(4))) {};
    Place _place {};
    uint32_t _received { 0 };   // bytes of the upload, header + package
    bool _active { false };
    Stream _stream;
};

// A free gap of the flash for the package of a new head: [bottom, top)
struct Gap {
    uint32_t bottom;
    uint32_t top;
};

// The one place rule of every chip (libs/rbl_layout.h): the package at the top of the highest gap it fits, named in
// the header with the gap's bottom (the bootloader keeps the old head inside that gap). `limit` - the gap's top.
// nullptr, else the error
const char* place(espurna_rbl_t& header, const Gap* gaps, size_t count, uint32_t& limit);

// The one RBL upload of the board, the driver part of every chip (ota_<chip>.cpp): begin() after the chip's own checks
// of the header and its place, then write() / end() / abort() of ota_driver.h as they are. An error drops the upload
namespace upload {

const char* begin(const Flash& flash, const espurna_rbl_t& header, const Place& place);
const char* write(const uint8_t* data, size_t len);
const char* end();
void abort();

} // namespace upload

// The new head confirms itself (owner 20.11, every chip of the scheme): the bootloader swapped the heads and keeps the
// old one compressed; 60 s of running with Wi-Fi and the header sector at `header_at` is erased - the swap is final.
// Without that the bootloader counts the starts and puts the old head back after ESPURNA_RBL_STARTS of them. Once at
// boot (driver::confirm() of the chip)
void confirm(const Flash& flash, uint32_t header_at);

} // namespace rbl
} // namespace ota
} // namespace espurna
