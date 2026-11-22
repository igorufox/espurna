/*

OTA MODULE, ESP8266 on ESP8266_RTOS_SDK: a new head and the bootloader over the network (owner 2026-11-18: the Tuya
bootloader's scheme, and the bootloader may be flashed explicitly)

/upgrade with the RBL of a new head (espurna-esp8266.ota, rbl_esp8266.h): ota.cpp hands it to driver::begin() .. end() here (ota_driver.h). The same
route takes an RBL of an image of the flash from 0x0 (scripts/esp8266_rbl.py --flash: ESPurna 1.15 / 1.22 of the Arduino
core back, owner 21.11) - the bootloader writes it over itself, no rollback. The modules
are off (ota_web.cpp refuses the upload while they run: the package goes where they are). The package is written right
under the header sector, read back for its CRC, and the 96 bytes of the header go last: before that the bootloader
finds nothing to install. The restart (ota_web.cpp) lets the bootloader install it (bootloader/espurna_rbl.c).

/upgrade/bootloader with the SDK's bootloader (bootloader.bin of the build): only on this explicit route. The file goes
to the same free space first and is checked there whole (the image magic, its segments, their checksum, the size of
the bootloader's place 0x0..0x8000); the flash mode and size bytes of the bootloader on the board are kept (as esptool
writes them). Only then it is copied to 0x0, a sector at a time: the one moment a power cut costs the wire.

*/

#include "espurna.h"

#include "ota_driver.h"
#include "rbl_esp8266.h"
#include "ota_rbl.h"

#if MODULES_SUPPORT
#include "modules.h"
#endif

#if WEB_SUPPORT
#include "web.h"
#include "libs/image_receiver.h"
#endif

#include <spi_flash.h>

#include <algorithm>
#include <cstring>
#include <memory>

namespace espurna {
namespace ota {
namespace driver {
namespace {

// the SDK's flash calls for the common writer (ota_rbl.h): sector erase, write and read of 4-byte multiples
bool flashErase(uint32_t at) {
    return spi_flash_erase_sector(at / ESPURNA_RBL_SECTOR) == ESP_OK;
}

bool flashWrite(uint32_t at, const uint8_t* data, size_t size) {
    return spi_flash_write(at, reinterpret_cast<const uint32_t*>(data), size) == ESP_OK;
}

bool flashRead(uint32_t at, uint8_t* data, size_t size) {
    return spi_flash_read(at, reinterpret_cast<uint32_t*>(data), size) == ESP_OK;
}

} // namespace

const rbl::Flash Flash { flashErase, flashWrite, flashRead };

namespace {

// the end of the running head, rounded to a sector: the free space starts there
uint32_t headEnd() {
    return espurna_rbl_round(ESPURNA_RBL_HEAD_AT + ::espurna::core::chip::sketch_size());
}

// from the heap for one upload (636 bytes held for good before, 11-18 memory pass)
std::unique_ptr<rbl::Writer> current;

} // namespace

// the one image this chip takes over the network: the RBL (one app partition, nothing for a plain app image to go to).
// The common part (ota_rbl.h) writes it; here only where it goes and what this chip's bootloader installs
const char* begin(const uint8_t* data, size_t len, size_t) {
    current.reset();

    espurna_rbl_t header {};
    const char* error = rbl::header(data, len, header);
    if (error) {
        return (header.magic == ESPURNA_RBL_MAGIC)
            ? error
            : PSTR("ERROR: Not an RBL of this chip (espurna-esp8266.ota, or esp8266_rbl.py --flash of an image)");
    }

    // a chip of 2 MB and more: the top of the flash (out of the modules' area), named in the header - before the check,
    // which tells whether the image and the package fit where the package goes (21.11: an image of the flash of 585 KB
    // was refused by the 1 MB place of its package). Naming the place signs the header again
    const uint32_t top_at = espurna_rbl_top_at(spi_flash_get_chip_size(), header.data_size);
    if (top_at) {
        espurna_rbl_set_package_at(&header, top_at);
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
        .limit = top_at ? (top_at + espurna_rbl_round(header.data_size)) : ESPURNA_RBL_HEADER_AT,
    };

    error = current->begin(Flash, header, place);
    if (error) {
        current.reset();
    }

    return error;
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
    return error;
}

void abort() {
    current.reset();
}

} // namespace driver
} // namespace ota
} // namespace espurna

// --- the bootloader, explicitly ----------------------------------------------------------------------------------------

#if WEB_SUPPORT && OTA_WEB_SUPPORT

namespace espurna {
namespace ota {
namespace bootloader {
namespace {

constexpr uint32_t Place { 0x8000 };            // 0x0 .. the partition table
constexpr uint32_t StageAt { ESPURNA_RBL_HEADER_AT - Place };

// the image format of the ESP8266 (esptool): magic, segment count, flash mode, size/frequency, entry; segments of
// address + length + data; the checksum (0xEF xor every data byte) in the last byte of the 16-byte padding
const char* checkImage(uint32_t at, uint32_t size) {
    uint8_t header[8];
    if ((size < 16) || (spi_flash_read(at, header, sizeof(header)) != ESP_OK)) {
        return PSTR("ERROR: Not an image");
    }

    if (header[0] != 0xE9) {
        return PSTR("ERROR: Not an ESP8266 image (magic)");
    }

    std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[512]);
    if (!buffer) {
        return PSTR("ERROR: Out of memory");
    }

    uint8_t sum { 0xEF };
    uint32_t offset { 8 };
    for (uint8_t segment = 0; segment < header[1]; ++segment) {
        uint32_t head[2];
        if (((offset + 8) > size) || (spi_flash_read(at + offset, head, sizeof(head)) != ESP_OK)) {
            return PSTR("ERROR: Image segments past its end");
        }
        offset += 8;

        uint32_t left = head[1];
        if ((offset + left) > size) {
            return PSTR("ERROR: Image segments past its end");
        }

        while (left) {
            const uint32_t take = std::min<uint32_t>(left, 512);
            if (spi_flash_read(at + offset, buffer.get(), (take + 3) & ~uint32_t{3}) != ESP_OK) {
                return PSTR("ERROR: Flash read failed");
            }
            for (uint32_t index = 0; index < take; ++index) {
                sum ^= buffer[index];
            }
            offset += take;
            left -= take;
        }
    }

    const uint32_t end = (offset + 16) & ~uint32_t{15};
    if (end > size) {
        return PSTR("ERROR: Image checksum past its end");
    }

    uint8_t last[4];
    if (spi_flash_read(at + end - 4, last, sizeof(last)) != ESP_OK) {
        return PSTR("ERROR: Flash read failed");
    }

    if (last[3] != sum) {
        return PSTR("ERROR: Image checksum");
    }

    return nullptr;
}

// the staged image onto 0x0, the flash mode and size bytes of the bootloader there kept
const char* install(uint32_t size) {
    uint8_t current[4];
    if (spi_flash_read(0, current, sizeof(current)) != ESP_OK) {
        return PSTR("ERROR: Flash read failed");
    }

    std::unique_ptr<uint8_t[]> sector(new (std::nothrow) uint8_t[ESPURNA_RBL_SECTOR]);
    if (!sector) {
        return PSTR("ERROR: Out of memory");
    }

    for (uint32_t at = 0; at < size; at += ESPURNA_RBL_SECTOR) {
        const uint32_t take = std::min<uint32_t>(size - at, ESPURNA_RBL_SECTOR);
        std::memset(sector.get(), 0xFF, ESPURNA_RBL_SECTOR);
        if (spi_flash_read(StageAt + at, sector.get(), (take + 3) & ~uint32_t{3}) != ESP_OK) {
            return PSTR("ERROR: Flash read failed");
        }

        if (!at && (current[0] == 0xE9)) {
            sector[2] = current[2];
            sector[3] = current[3];
        }

        if ((spi_flash_erase_sector(at / ESPURNA_RBL_SECTOR) != ESP_OK)
            || (spi_flash_write(at, sector.get(), (take + 3) & ~uint32_t{3}) != ESP_OK))
        {
            return PSTR("ERROR: Flash write failed - the board needs the wire now");
        }
    }

    return nullptr;
}

class Target final : public espurna::image::Target {
public:
    const char* begin(const uint8_t* data, size_t len, size_t size) override {
        if ((len < 8) || (data[0] != 0xE9)) {
            return PSTR("not an ESP8266 bootloader image");
        }

        if (size > Place) {
            return PSTR("larger than the bootloader's place (32 KB)");
        }

        _writer.start(driver::Flash, StageAt, StageAt + Place);
        _active = true;
        return nullptr;
    }

    const char* write(const uint8_t* data, size_t len) override {
        if (!_active || !_writer.push(data, len)) {
            _active = false;
            return PSTR("flash write failed");
        }
        return nullptr;
    }

    const char* end(size_t size) override {
        if (!_active || !_writer.flush()) {
            _active = false;
            return PSTR("flash write failed");
        }
        _active = false;

        const char* error = checkImage(StageAt, size);
        if (!error) {
            error = install(size);
        }

        if (!error) {
            DEBUG_MSG_P(PSTR("[OTA] Bootloader written, %u bytes\n"), static_cast<unsigned>(size));
        }

        return error;
    }

    void abort() override {
        _active = false;
    }

private:
    rbl::Stream _writer;
    bool _active { false };
};

struct Upload {
    Target target;
    espurna::image::Receiver receiver { target };
    int code { 0 };
    espurna::string error;
};

Upload* uploadOf(WebRequest& request) {
    auto* upload = static_cast<Upload*>(request.context());
    if (!upload) {
        upload = new (std::nothrow) Upload();
        request.setContext(upload, [](void* ptr) {
            delete static_cast<Upload*>(ptr);
        });
    }
    return upload;
}

void onFile(WebRequest& request, const char*, size_t index, const uint8_t* data, size_t len, bool) {
    auto* upload = uploadOf(request);
    if (!upload || upload->code) {
        return;
    }

    if (!index) {
        if (!webAuthenticate(request)) {
            upload->code = 401;
            return;
        }

        if (!espurna::image::sameOrigin(
            request.header("Origin"), request.header("Host"), request.header("X-Forwarded-Host")))
        {
            upload->code = 403;
            upload->error = espurna::string("ERROR: Origin");
            return;
        }

#if MODULES_SUPPORT
        // the image is staged where the modules are
        if (modulesRunning()) {
            upload->code = 409;
            upload->error = espurna::string("ERROR: modules installed, MODULES.OFF first");
            return;
        }
#endif

        upload->receiver.expect(request.fileSize());
    }

    const char* error = upload->receiver.push(data, len);
    if (error) {
        upload->code = 400;
        upload->error = espurna::string("ERROR: ") + error;
    }
}

void onDone(WebRequest& request) {
    if (!webAuthenticate(request)) {
        webRequestAuthentication(request);
        return;
    }

    auto* upload = uploadOf(request);
    if (!upload) {
        webSend(request, 500, "text/plain", espurna::string("ERROR: out of memory"));
        return;
    }

    if (upload->code) {
        webSend(request, upload->code, "text/plain",
            upload->error.length() ? upload->error : espurna::string("ERROR"));
        return;
    }

    const char* error = upload->receiver.finish();
    if (error) {
        webSend(request, 400, "text/plain", espurna::string("ERROR: ") + error);
        return;
    }

    webSend(request, 200, "text/plain", espurna::string("OK, the bootloader is written"));
}

} // namespace
} // namespace bootloader
} // namespace ota
} // namespace espurna

void espurna::ota::driver::web_setup() {
    webServer().on("/upgrade/bootloader", WebMethod::Post,
        espurna::ota::bootloader::onDone, espurna::ota::bootloader::onFile);
}

#else

void espurna::ota::driver::web_setup() {
}

#endif

// The new head confirms itself to the bootloader: the common part (ota_rbl.h), at the header sector of this chip
void espurna::ota::driver::confirm() {
    rbl::confirm(Flash, ESPURNA_RBL_HEADER_AT);
}
