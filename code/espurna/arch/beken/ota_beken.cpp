/*

OTA MODULE, BK7231N: which bootloader a .uf2 upload is for (ref. ota_driver_chip.h)

One head image, two initial images of it: espurna-beken-tuya.uf2 for the Tuya bootloader and espurna-beken-quectel.uf2
for the Quectel one (scripts/beken_quectel_image.py). Each UF2 carries its own FAL partition table (tag FAL_PTABLE)
and LibreTiny Update writes the 'download' partition of that table, not of the one this firmware was built with.
So the board looks at that table before anything is written: its 'download' partition has to be the one of this
board's bootloader (layout_beken.cpp). The other bootloader's file would land in the application, or the bootloader
would install an image in the wrong format (the Tuya one is encrypted, the Quectel one is not): refused.

*/

#include "espurna.h"

#include "layout_beken.h"
#include "../../ota_driver.h"

#include <cstring>

#include <fal.h>

namespace espurna {
namespace ota {
namespace driver {
namespace {

// UF2 block: 32 byte header, 476 bytes of data and tags, the last magic word
constexpr size_t BlockSize { 512 };
constexpr size_t HeaderSize { 32 };
constexpr size_t DataSize { 476 };

constexpr uint32_t FlagHasMd5 { 1u << 14 };
constexpr uint32_t FlagHasTags { 1u << 15 };

// uf2ota/uf2types.h
constexpr uint32_t TagFalPtable { 0x8288ED };

// struct fal_partition of FlashDB with FAL_DEV_NAME_MAX 16 (LibreTiny's fal_cfg.h)
constexpr size_t PartitionSize { 48 };
constexpr size_t PartitionName { 4 };
constexpr size_t PartitionOffset { 36 };
constexpr size_t PartitionLength { 40 };
constexpr size_t NameMax { 16 };

uint32_t word(const uint8_t* data) {
    return uint32_t{data[0]}
        | (uint32_t{data[1]} << 8)
        | (uint32_t{data[2]} << 16)
        | (uint32_t{data[3]} << 24);
}

// the 'download' partition of the first block's FAL_PTABLE tag; false when there is none
bool download(const uint8_t* block, uint32_t& offset, uint32_t& length) {
    const auto flags = word(&block[8]);
    if (!(flags & FlagHasTags)) {
        return false;
    }

    const auto size = word(&block[16]);
    if (size > DataSize) {
        return false;
    }

    const uint8_t* tags = &block[HeaderSize + size];
    const uint8_t* end = &block[HeaderSize + DataSize - ((flags & FlagHasMd5) ? 24 : 0)];
    while ((tags + 4) <= end) {
        const size_t len = tags[0];
        const uint32_t type = uint32_t{tags[1]} | (uint32_t{tags[2]} << 8) | (uint32_t{tags[3]} << 16);
        // a tag is at least its own 4 byte header: a shorter length wrapped `len - 4` below (review M1)
        if ((len < 4) || !type || ((tags + len) > end)) {
            break;
        }

        if (type == TagFalPtable) {
            const uint8_t* entry = tags + 4;
            for (size_t left = len - 4; left >= PartitionSize; left -= PartitionSize, entry += PartitionSize) {
                const char* name = reinterpret_cast<const char*>(&entry[PartitionName]);
                if (strncmp(name, "download", NameMax) == 0) {
                    offset = word(&entry[PartitionOffset]);
                    length = word(&entry[PartitionLength]);
                    return true;
                }
            }
            return false;
        }

        tags += ((len + 3) / 4) * 4;
    }

    return false;
}

} // namespace

const char* upload_check(const uint8_t* data, size_t len) {
    const auto& layout = ::espurna::beken::flashLayout();
    // an unknown bootloader: where its download partition is, is not known - nothing is written (layout_beken.cpp)
    if (layout.layout == ::espurna::beken::Layout::Unknown) {
        return PSTR("ERROR: Unknown bootloader, no OTA on this board (flash it by wire)");
    }

    if (!layout.installs_rbl) {
        return PSTR("ERROR: Tuya bootloader 3.00 does not install this image, update over UART or replace the bootloader");
    }

    if (len < BlockSize) {
        return PSTR("ERROR: Upload too short for an UF2 block");
    }

    uint32_t offset { 0 };
    uint32_t length { 0 };
    if (!download(data, offset, length)) {
        return PSTR("ERROR: The .uf2 has no partition table, the bootloader it is for is not known");
    }

    if ((offset != layout.download_offset) || (length != layout.download_length)) {
        return (layout.layout == ::espurna::beken::Layout::Quectel)
            ? PSTR("ERROR: This .uf2 is for the Tuya bootloader, this board has the Quectel one: espurna-beken-quectel.uf2")
            : PSTR("ERROR: This .uf2 is for the Quectel bootloader, this board has the Tuya one: espurna-beken-tuya.uf2");
    }

    return nullptr;
}

// --- the RBL head of the install image ---------------------------------------------------------------------------
// The .ota carries the head as the RBL package only (~450 KB; the .uf2 of the wire around it was 2.5 / 4.5 MB, W62),
// the same package for both bootloaders. It goes into 'download' of this board's layout; its first bytes (the RBL
// magic) are written last, so an interrupted upload leaves no package for the bootloader to install.

namespace {

constexpr uint32_t RblMagic { 0x004C4252 }; // "RBL\0"
constexpr size_t RblHeader { 96 };          // ltchiptool bk72xx RBL: magic .. size_package (at 88), info_crc32
constexpr size_t RblFirst { 16 };
constexpr uint32_t Sector { 0x1000 };

struct Rbl {
    bool active { false };
    uint32_t offset { 0 };      // 'download' of this layout
    uint32_t length { 0 };      // header + package
    uint32_t written { 0 };
    uint32_t erased { 0 };      // bytes of 'download' erased so far
    uint8_t first[RblFirst] {};
};

Rbl rbl;

void rbl_drop() {
    if (rbl.erased) {
        lt_flash_erase_block(rbl.offset);
    }
    rbl = Rbl{};
}

} // namespace

bool own_image(const uint8_t* data, size_t len) {
    return (len >= 4) && (word(data) == RblMagic);
}

const char* own_begin(const uint8_t* data, size_t len) {
    const auto& layout = ::espurna::beken::flashLayout();
    if (layout.layout == ::espurna::beken::Layout::Unknown) {
        return PSTR("ERROR: Unknown bootloader, no OTA on this board (flash it by wire)");
    }

    if (!layout.installs_rbl) {
        return PSTR("ERROR: Tuya bootloader 3.00 does not install this image, update over UART or replace the bootloader");
    }

    if (len < RblHeader) {
        return PSTR("ERROR: Upload too short for an RBL header");
    }

    const uint32_t length = RblHeader + word(&data[88]);
    if (length > layout.download_length) {
        return PSTR("ERROR: The image does not fit the download partition");
    }

    // nothing is erased when the flash chip does not reach the end of 'download' (system_beken.cpp, __wrap_lt_flash_*)
    if ((layout.download_offset + length) > lt_flash_get_size()) {
        return PSTR("ERROR: The download partition is past the flash this head can write");
    }

    rbl = Rbl{};
    rbl.active = true;
    rbl.offset = layout.download_offset;
    rbl.length = length;
    return nullptr;
}

const char* own_write(const uint8_t* data, size_t len) {
    if (!rbl.active) {
        return PSTR("ERROR: No upload");
    }

    if ((rbl.written + len) > rbl.length) {
        rbl_drop();
        return PSTR("ERROR: More data than the RBL header says");
    }

    while (rbl.erased < (rbl.written + len)) {
        if (!lt_flash_erase_block(rbl.offset + rbl.erased)) {
            rbl_drop();
            return PSTR("ERROR: Flash erase failed");
        }
        rbl.erased += Sector;
    }

    // the first bytes wait for the end
    if (rbl.written < RblFirst) {
        const size_t keep = std::min<size_t>(RblFirst - rbl.written, len);
        std::memcpy(&rbl.first[rbl.written], data, keep);
        rbl.written += keep;
        data += keep;
        len -= keep;
    }

    if (len) {
        if (lt_flash_write(rbl.offset + rbl.written, data, len) != len) {
            rbl_drop();
            return PSTR("ERROR: Flash write failed");
        }
        rbl.written += len;
    }

    return nullptr;
}

const char* own_end() {
    if (!rbl.active || (rbl.written != rbl.length)) {
        rbl_drop();
        return PSTR("ERROR: Incomplete image");
    }

    if (lt_flash_write(rbl.offset, rbl.first, RblFirst) != RblFirst) {
        rbl_drop();
        return PSTR("ERROR: Flash write failed");
    }

    uint32_t magic { 0 };
    lt_flash_read(rbl.offset, reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
    if (magic != RblMagic) {
        rbl_drop();
        return PSTR("ERROR: Not an OTA package");
    }

    DEBUG_MSG_P(PSTR("[OTA] RBL written, %u bytes\n"), rbl.written);
    rbl = Rbl{};
    return nullptr;
}

void own_abort() {
    if (rbl.active) {
        rbl_drop();
    }
}

} // namespace driver
} // namespace ota
} // namespace espurna

