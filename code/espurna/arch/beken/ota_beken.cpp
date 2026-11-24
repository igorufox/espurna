/*

OTA MODULE, BK7231N (ref. ota_driver.h): the RBL of a new head into 'download', the .uf2 of the wire, and which
bootloader a .uf2 upload is for

One head image, two initial images of it: espurna-beken-tuya.uf2 for the Tuya bootloader and espurna-beken-quectel.uf2
for the Quectel one (scripts/beken_quectel_image.py). Each UF2 carries its own FAL partition table (tag FAL_PTABLE)
and the UF2 OTA (chip::ota_* below) writes the 'download' partition of that table, not of the one this firmware was built with.
So the board looks at that table before anything is written: its 'download' partition has to be the one of this
board's bootloader (layout_beken.cpp). The other bootloader's file would land in the application, or the bootloader
would install an image in the wrong format (the Tuya one is encrypted, the Quectel one is not): refused.

*/

#include "beken_api.h"
#include "../../espurna.h"

#include "layout_beken.h"
#include "../../ota_driver.h"
#include "../../ota_rbl.h"

#include <cstring>



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

// struct fal_partition of FlashDB with FAL_DEV_NAME_MAX 16 (the UF2 format, flashdb/fal_cfg.h)
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

const char* uf2_check(const uint8_t* data, size_t len) {
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
// the same package for both bootloaders. The common writer (ota_rbl.h) puts it into 'download' of this board's layout,
// the header first in the partition and written last: an interrupted upload leaves no package for the bootloader.
// Here only the flash calls, where 'download' is and what this board's bootloader installs

namespace {

bool flashErase(uint32_t at) {
    return ::espurna::beken::flash_erase_block(at);
}

bool flashWrite(uint32_t at, const uint8_t* data, size_t size) {
    return ::espurna::beken::flash_write(at, data, size) == size;
}

bool flashRead(uint32_t at, uint8_t* data, size_t size) {
    return ::espurna::beken::flash_read(at, data, size) == size;
}

const rbl::Flash Flash { flashErase, flashWrite, flashRead };

} // namespace

bool rbl_magic(const uint8_t* data, size_t len) {
    return (len >= 4) && (word(data) == ESPURNA_RBL_MAGIC);
}

const char* rbl_begin(const uint8_t* data, size_t len) {
    rbl::upload::abort();

    const auto& layout = ::espurna::beken::flashLayout();
    if (layout.layout == ::espurna::beken::Layout::Unknown) {
        return PSTR("ERROR: Unknown bootloader, no OTA on this board (flash it by wire)");
    }

    if (!layout.installs_rbl) {
        return PSTR("ERROR: Tuya bootloader 3.00 does not install this image, update over UART or replace the bootloader");
    }

    espurna_rbl_t header {};
    const char* error = rbl::header(data, len, header);
    if (error) {
        return error;
    }

    // the package of this chip only (scripts/beken_image.py package_ota): the BDK's "app", AES-256-CBC (2) of a gzip
    // (1). The RBL of the ESP chips is another name and plain deflate chunks
    if ((std::strncmp(header.name, "app", sizeof(header.name)) != 0)
        || (header.encryption != 2) || (header.compression != 1))
    {
        return PSTR("ERROR: Not an RBL of this chip (espurna-beken.ota)");
    }

    const uint32_t length = ESPURNA_RBL_SIZE + header.data_size;
    if (length > layout.download_length) {
        return PSTR("ERROR: The image does not fit the download partition");
    }

    // nothing is erased when the flash chip does not reach the end of 'download' (system_beken.cpp, __wrap_lt_flash_*)
    if ((layout.download_offset + length) > ::espurna::beken::flash_size()) {
        return PSTR("ERROR: The download partition is past the flash this head can write");
    }

    const rbl::Place place {
        .header = layout.download_offset,
        .package = layout.download_offset + ESPURNA_RBL_SIZE,
        .limit = layout.download_offset + layout.download_length,
    };

    return rbl::upload::begin(Flash, header, place);
}

const char* rbl_write(const uint8_t* data, size_t len) {
    return rbl::upload::write(data, len);
}

const char* rbl_end() {
    return rbl::upload::end();
}

void rbl_abort() {
    rbl::upload::abort();
}

} // namespace driver
} // namespace ota

// --- the .uf2 of the wire (chip::ota_* of chip_sdk.h) --------------------------------------------------------------
// LibreTiny's UF2 OTA format (its uf2ota library did this until 2026-11-19): 512 byte blocks, the first one with the
// tags only; OTA_PART_INFO names the partition of the blocks that follow for every OTA scheme (the Beken one is
// "device, single": the high nibble of the first byte), the FAL_PTABLE tag of the first block where each name is.
// uf2_check() has made sure that table's 'download' is the one of this board's bootloader.

namespace core {
namespace chip {
namespace {

using ota::driver::word;

constexpr uint32_t UfMagic1 { 0x0A324655 };
constexpr uint32_t UfMagic2 { 0x9E5D5157 };
constexpr uint32_t UfMagic3 { 0x0AB16F30 };
constexpr uint32_t FlagNotMainFlash { 1u << 0 };
constexpr uint32_t FlagFileContainer { 1u << 12 };
constexpr uint32_t FlagFamilyId { 1u << 13 };
constexpr uint32_t FamilyBk7231n { 0x7B3EF230 };

constexpr uint32_t TagOtaFormat2 { 0x6C8492 };
constexpr uint32_t TagOtaPartInfo { 0xC0EE0C };

struct Partition {
    char name[17];
    uint32_t offset;
    uint32_t length;
};

struct Uf2 {
    bool active { false };
    uint32_t seq { 0 };
    bool format_ok { false };
    Partition table[8] {};
    size_t table_len { 0 };
    const Partition* part { nullptr };
    bool part_set { false };
    uint32_t erased_end { 0 };
    uint32_t written { 0 };
    uint8_t block[ota::driver::BlockSize] {};
    size_t filled { 0 };
};

Uf2* uf2 { nullptr };

const char* uf2_partition(const uint8_t* tag, size_t tag_len) {
    uf2->part = nullptr;
    uf2->part_set = true;
    if (tag_len < 3) {
        return PSTR("ERROR: UF2 partition info");
    }

    const uint8_t index = (tag[0] >> 4) & 0xF;
    if (!index) {
        return nullptr;
    }

    const char* name = reinterpret_cast<const char*>(tag) + 3;
    const char* end = reinterpret_cast<const char*>(tag) + tag_len;
    for (uint8_t current = 1; name < end; ++current) {
        const char* stop = static_cast<const char*>(std::memchr(name, '\0', end - name));
        if (!stop || (stop == name)) {
            break;
        }

        if (current == index) {
            for (size_t entry = 0; entry < uf2->table_len; ++entry) {
                if (std::strcmp(uf2->table[entry].name, name) == 0) {
                    uf2->part = &uf2->table[entry];
                    return nullptr;
                }
            }
            return PSTR("ERROR: UF2 partition not in its table");
        }

        name = stop + 1;
    }

    return PSTR("ERROR: UF2 partition info");
}

void uf2_table(const uint8_t* tag, size_t tag_len) {
    using namespace ota::driver;
    uf2->table_len = 0;
    for (const uint8_t* entry = tag; (tag_len >= PartitionSize) && (uf2->table_len < std::size(uf2->table));
        tag_len -= PartitionSize, entry += PartitionSize)
    {
        auto& out = uf2->table[uf2->table_len++];
        std::memset(out.name, 0, sizeof(out.name));
        std::memcpy(out.name, &entry[PartitionName], NameMax);
        out.offset = word(&entry[PartitionOffset]);
        out.length = word(&entry[PartitionLength]);
    }
}

const char* uf2_tags(const uint8_t* block) {
    using namespace ota::driver;
    const auto flags = word(&block[8]);
    if (!(flags & FlagHasTags)) {
        return nullptr;
    }

    const auto size = word(&block[16]);
    if (size > (DataSize - 8)) {
        return PSTR("ERROR: UF2 block data too long");
    }

    const uint8_t* tags = &block[HeaderSize + size];
    const uint8_t* end = &block[HeaderSize + DataSize - ((flags & FlagHasMd5) ? 24 : 0)];
    while ((tags + 4) <= end) {
        const size_t len = tags[0];
        const uint32_t type = uint32_t{tags[1]} | (uint32_t{tags[2]} << 8) | (uint32_t{tags[3]} << 16);
        if ((len < 4) || !type || ((tags + len) > end)) {
            break;
        }

        if (type == TagOtaFormat2) {
            uf2->format_ok = true;
        } else if (type == TagFalPtable) {
            uf2_table(tags + 4, len - 4);
        } else if (type == TagOtaPartInfo) {
            const auto* error = uf2_partition(tags + 4, len - 4);
            if (error) {
                return error;
            }
        }

        tags += ((len + 3) / 4) * 4;
    }

    return nullptr;
}

const char* uf2_block(const uint8_t* block) {
    using namespace ota::driver;
    if ((word(&block[0]) != UfMagic1) || (word(&block[4]) != UfMagic2) || (word(&block[508]) != UfMagic3)) {
        return PSTR("ERROR: Not an UF2 block");
    }

    const auto flags = word(&block[8]);
    if (flags & FlagFileContainer) {
        return nullptr;
    }

    if (!(flags & FlagFamilyId) || (word(&block[28]) != FamilyBk7231n)) {
        return PSTR("ERROR: UF2 of another chip");
    }

    if (word(&block[20]) != uf2->seq) {
        return PSTR("ERROR: UF2 block out of order");
    }

    ++uf2->seq;
    const auto* error = uf2_tags(block);
    if (error) {
        return error;
    }

    const uint32_t address = word(&block[12]);
    const uint32_t size = word(&block[16]);
    if ((uf2->seq == 1) && !uf2->format_ok) {
        return PSTR("ERROR: UF2 OTA format not supported");
    }

    if ((flags & FlagNotMainFlash) || !size) {
        return nullptr;
    }

    if (!uf2->part_set) {
        return PSTR("ERROR: UF2 partition not set");
    }

    // a block of another OTA scheme
    if (!uf2->part) {
        return nullptr;
    }

    if ((address + size) > uf2->part->length) {
        return PSTR("ERROR: UF2 block past its partition");
    }

    const uint32_t offset = uf2->part->offset + address;
    while (uf2->erased_end < (offset + size)) {
        const uint32_t sector = std::max(uf2->erased_end, offset & ~(ota::rbl::Sector - 1));
        if (!beken::flash_erase_block(sector)) {
            return PSTR("ERROR: Flash erase failed");
        }
        uf2->erased_end = sector + ota::rbl::Sector;
    }

    if (beken::flash_write(offset, &block[HeaderSize], size) != size) {
        return PSTR("ERROR: Flash write failed");
    }

    uf2->written += size;
    return nullptr;
}

} // namespace

const char* ota_begin(size_t) {
    ota_abort();
    uf2 = new (std::nothrow) Uf2();
    if (!uf2) {
        return PSTR("ERROR: Not enough memory");
    }

    uf2->active = true;
    return nullptr;
}

const char* ota_write(const uint8_t* data, size_t size) {
    if (!uf2) {
        return PSTR("ERROR: No upload");
    }

    while (size) {
        const size_t take = std::min(size, sizeof(uf2->block) - uf2->filled);
        std::memcpy(&uf2->block[uf2->filled], data, take);
        uf2->filled += take;
        data += take;
        size -= take;

        if (uf2->filled == sizeof(uf2->block)) {
            uf2->filled = 0;
            const auto* error = uf2_block(uf2->block);
            if (error) {
                return error;
            }
        }
    }

    return nullptr;
}

const char* ota_end() {
    if (!uf2) {
        return PSTR("ERROR: No upload");
    }

    const bool ok = uf2->written && !uf2->filled;
    delete uf2;
    uf2 = nullptr;
    return ok ? nullptr : PSTR("ERROR: Incomplete UF2");
}

void ota_abort() {
    delete uf2;
    uf2 = nullptr;
}

} // namespace chip
} // namespace core
} // namespace espurna

// --- the one path of ota_driver.h: the RBL of a head, or the .uf2 of the wire --------------------------------------

namespace espurna {
namespace ota {
namespace driver {
namespace {

enum class Mode {
    None,
    Rbl,
    Uf2,
};

Mode mode { Mode::None };

bool uf2_magic(const uint8_t* data, size_t len) {
    return (len >= 8) && (word(data) == 0x0A324655) && (word(&data[4]) == 0x9E5D5157);
}

} // namespace

const char* begin(const uint8_t* data, size_t len, size_t) {
    abort();

    if (rbl_magic(data, len)) {
        const auto* error = rbl_begin(data, len);
        if (!error) {
            mode = Mode::Rbl;
        }
        return error;
    }

    if (!uf2_magic(data, len)) {
        return PSTR("ERROR: Not a firmware image for this chip (RBL of the .ota, or an UF2 block)");
    }

    // which bootloader the .uf2 is for, before anything is written
    const auto* error = uf2_check(data, len);
    if (error) {
        return error;
    }

    // the .uf2 is a lot bigger than the image it carries, the UF2 writer checks the partition itself
    error = ::espurna::core::chip::ota_begin(::espurna::core::chip::OtaSizeUnknown);
    if (!error) {
        mode = Mode::Uf2;
    }
    return error;
}

const char* write(const uint8_t* data, size_t len) {
    switch (mode) {
    case Mode::Rbl:
        return rbl_write(data, len);
    case Mode::Uf2:
        return ::espurna::core::chip::ota_write(data, len);
    case Mode::None:
        break;
    }

    return PSTR("ERROR: No upload");
}

const char* end() {
    const auto was = mode;
    mode = Mode::None;

    switch (was) {
    case Mode::Rbl:
        return rbl_end();
    case Mode::Uf2:
        return ::espurna::core::chip::ota_end();
    case Mode::None:
        break;
    }

    return PSTR("ERROR: No upload");
}

void abort() {
    switch (mode) {
    case Mode::Rbl:
        rbl_abort();
        break;
    case Mode::Uf2:
        ::espurna::core::chip::ota_abort();
        break;
    case Mode::None:
        break;
    }

    mode = Mode::None;
}

// the bootloader installs the package and boots it, nothing to confirm
void confirm() {
}

void web_setup() {
}

} // namespace driver
} // namespace ota
} // namespace espurna
