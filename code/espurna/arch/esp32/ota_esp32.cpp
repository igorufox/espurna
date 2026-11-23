/*

OTA MODULE, ESP32 (ref. ota_driver.h): the ESP8266's scheme (owner 22.11: «делай как ESP8266 у нас общая схема», one
app partition): /upgrade takes the RBL of a new head (espurna-esp32.ota's head, rbl_esp32.h), the common writer
(ota_rbl.h) puts the package right under the header sector of app0 and the header last; the bootloader step
(arch/rtos/bootloader/espurna_rbl.c, code/bootloader_components/espurna_rbl) swaps the heads on the next boot and keeps
the old one until this one confirms itself. The modules are off while it is written (ota_web.cpp: the package goes
where they are). A plain app image (0xE9) is refused: there is no second app partition to put it into.

The migration both ways (owner 22.11, scripts/esp32_migrate.py): /upgrade also takes an image of the flash from 0x0
(ESPURNA_RBL_FLASH), the bootloader writes it. On the safeboot layout of the Arduino builds this head runs from app0 at
0xE0000 under their bootloader, which has no RBL step: then the head writes the new bootloader itself, out of the
package (its boot sectors are stored deflate blocks), after the header; the new bootloader writes the rest.

*/

#include "espurna.h"

#include "ota_driver.h"
#include "ota_rbl.h"
#include "rbl_esp32.h"

#include "../../modules.h"
#include "../../modules_core.h"

#include <chip_sdk.h>

#include <esp_ota_ops.h>

#include <cstring>
#include <memory>

namespace espurna {
namespace ota {
namespace driver {
namespace {

// the flash of app0 through the module loader's calls (modules_esp32.cpp: esp_flash_write() refuses the running app
// partition, those write it)
bool flashErase(uint32_t at) {
    return ::espurna::modules::arch::rawErase(at, rbl::Sector);
}

bool flashWrite(uint32_t at, const uint8_t* data, size_t size) {
    return ::espurna::modules::arch::rawWrite(at, data, size);
}

bool flashRead(uint32_t at, uint8_t* data, size_t size) {
    return ::espurna::modules::arch::rawRead(at, data, size);
}

const rbl::Flash Flash { flashErase, flashWrite, flashRead };

// where the running head is: app0 at ESPURNA_RBL_HEAD_AT, or at 0xE0000 of the safeboot layout (the migration)
uint32_t headAt() {
    const auto* running = esp_ota_get_running_partition();
    return running ? running->address : ESPURNA_RBL_HEAD_AT;
}

// the end of the running head, rounded to a sector: the free space starts there
uint32_t headEnd() {
    return espurna_rbl_round(headAt() + ::espurna::core::chip::sketch_size());
}

std::unique_ptr<rbl::Writer> current;
espurna_rbl_t current_header {};
uint32_t current_package { 0 };

// --- the migration: the new bootloader written by the head --------------------------------------------------------

constexpr uint32_t BootloaderAt { 0x1000 };
constexpr uint32_t BootloaderEnd { 0x8000 };

// a chunk of the package (u16 length, then a raw deflate) that is one stored block of a whole sector: 0x01, LEN,
// NLEN, the bytes (scripts/esp8266_rbl.py STORED_BELOW)
constexpr size_t StoredChunk { 2 + 5 + rbl::Sector };

struct BootSector {
    uint32_t chunk;     // where its chunk starts in the flash
};

// the chunk after the one at `at` (u16 length, padded to 4 bytes)
bool chunkNext(uint32_t& at) {
    uint8_t word[4];
    if (!flashRead(at, word, sizeof(word))) {
        return false;
    }

    const uint32_t size = word[0] | (word[1] << 8);
    at += (2 + size + 3) & ~static_cast<uint32_t>(3);
    return true;
}

// the stored sector in the chunk at `at` into `out` (rbl::Sector bytes)
bool storedSector(uint32_t at, uint8_t* buffer, const uint8_t*& out) {
    const size_t size = (StoredChunk + 3) & ~static_cast<size_t>(3);
    if (!flashRead(at, buffer, size)) {
        return false;
    }

    const uint32_t length = buffer[0] | (buffer[1] << 8);
    const uint32_t len = buffer[3] | (buffer[4] << 8);
    const uint32_t nlen = buffer[5] | (buffer[6] << 8);
    if ((length != (StoredChunk - 2)) || (buffer[2] != 0x01) || (len != rbl::Sector) || ((len ^ 0xFFFF) != nlen)) {
        return false;
    }

    out = &buffer[7];
    return true;
}

// the image of the flash under another bootloader (the head is not at ESPURNA_RBL_HEAD_AT): its bootloader sectors
// (0x1000..0x8000) written here, checked first, each read back. A power cut in these few writes leaves no bootloader:
// the risk of every migration (the ESP8266's eboot copy too)
const char* migrationBootloader(const espurna_rbl_t& header, uint32_t package) {
    if ((header.compression != ESPURNA_RBL_FLASH) || (headAt() == ESPURNA_RBL_HEAD_AT)) {
        return nullptr;
    }

    if (header.raw_size < BootloaderEnd) {
        return PSTR("ERROR: The image of the flash has no bootloader");
    }

    std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[StoredChunk + 4]);
    std::unique_ptr<uint8_t[]> check(new (std::nothrow) uint8_t[rbl::Sector]);
    if (!buffer || !check) {
        return PSTR("ERROR: Not enough memory");
    }

    // the chunks of the bootloader's sectors, every one a stored sector, before anything is erased
    BootSector sectors[(BootloaderEnd - BootloaderAt) / rbl::Sector];
    uint32_t at = package;
    for (uint32_t offset = 0; offset < BootloaderEnd; offset += rbl::Sector) {
        if (offset >= BootloaderAt) {
            const uint8_t* data { nullptr };
            if (!storedSector(at, buffer.get(), data)) {
                return PSTR("ERROR: The bootloader of the image is not stored (esp32_migrate.py)");
            }
            if ((offset == BootloaderAt) && (data[0] != 0xE9)) {
                return PSTR("ERROR: No bootloader at 0x1000 of the image");
            }
            sectors[(offset - BootloaderAt) / rbl::Sector].chunk = at;
        }
        if (!chunkNext(at)) {
            return PSTR("ERROR: Flash read failed");
        }
    }

    DEBUG_MSG_P(PSTR("[OTA] Another bootloader: writing the one of the image (0x%05X..0x%05X)\n"),
        BootloaderAt, BootloaderEnd);
    for (size_t index = 0; index < (sizeof(sectors) / sizeof(sectors[0])); ++index) {
        const uint32_t offset = BootloaderAt + (index * rbl::Sector);
        const uint8_t* data { nullptr };
        bool ok = false;
        for (uint32_t attempt = 0; !ok && (attempt < 3); ++attempt) {
            ok = storedSector(sectors[index].chunk, buffer.get(), data)
                && flashErase(offset) && flashWrite(offset, data, rbl::Sector)
                && flashRead(offset, check.get(), rbl::Sector)
                && (std::memcmp(check.get(), data, rbl::Sector) == 0);
        }
        if (!ok) {
            return PSTR("ERROR: Writing the bootloader failed");
        }
    }

    return nullptr;
}

} // namespace

const char* begin(const uint8_t* data, size_t len, size_t) {
    current.reset();

    espurna_rbl_t header {};
    const char* error = rbl::header(data, len, header);
    if (error) {
        return (header.magic == ESPURNA_RBL_MAGIC)
            ? error
            : PSTR("ERROR: Not an RBL of this chip (the head of espurna-esp32.ota)");
    }

    // the package under the data of the modules (modulesData(): the tile pages of the screen module), named in the
    // header: right under the header sector it was written over that data and the data was gone after a head update.
    // The bootloader's zone for the old head ends at the package's top (zone_top()), under the data too
    uint32_t limit = ESPURNA_RBL_HEADER_AT;
    const uint32_t data_at = modulesDataBottom();
    if (data_at && (data_at < limit)) {
        const uint32_t size = espurna_rbl_round(header.data_size);
        if (size >= data_at) {
            return PSTR("ERROR: The package does not fit under the data of the modules");
        }
        espurna_rbl_set_package_at(&header, data_at - size);
        limit = data_at;
    }

    error = espurna_rbl_check(&header);
    if (error) {
        return error;
    }

    const uint32_t package_at = espurna_rbl_package_at(&header);
    if (package_at < headEnd()) {
        return PSTR("ERROR: The package does not fit above the running head");
    }

    current.reset(new (std::nothrow) rbl::Writer());
    if (!current) {
        return PSTR("ERROR: Not enough memory");
    }

    const rbl::Place place {
        .header = ESPURNA_RBL_HEADER_AT,
        .package = package_at,
        .limit = limit,
    };

    error = current->begin(Flash, header, place);
    if (error) {
        current.reset();
        return error;
    }

    current_header = header;
    current_package = package_at;
    return nullptr;
}

const char* write(const uint8_t* data, size_t len) {
    if (!current) {
        return PSTR("ERROR: No upload");
    }

    const auto* error = current->write(data, len);
    if (error) {
        current.reset();
    }

    return error;
}

const char* end() {
    if (!current) {
        return PSTR("ERROR: Incomplete image");
    }

    const auto* error = current->end();
    current.reset();
    if (!error) {
        error = migrationBootloader(current_header, current_package);
    }

    return error;
}

void abort() {
    current.reset();
}

// the new head confirms itself to the bootloader (the common part, ota_rbl.h)
void confirm() {
    rbl::confirm(Flash, ESPURNA_RBL_HEADER_AT);
}

void web_setup() {
}

} // namespace driver
} // namespace ota
} // namespace espurna
