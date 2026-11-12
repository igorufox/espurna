/*

BK7231N FLASH LAYOUT, PICKED AT BOOT

One image for the boards of both bootloaders. The bootloader is told apart by the CRC32 of its flash region, the same
on every board of a vendor (checked against the stock dumps: CB3S and CBU, two XH-CB2S; a second Tuya bootloader build on a slot board). An unknown bootloader gets
no layout at all: nothing is written to its partitions then (no OTA, no Matter code).

*/

#include "espurna.h"

#include "layout_beken.h"
#include "../../libs/crc32_ieee.h"

namespace espurna {
namespace beken {
namespace {

constexpr uint32_t BootloaderSize { 0x11000 };

struct Known {
    uint32_t crc;
    FlashLayout layout;
};

constexpr Known KnownLayouts[] {
    {0xf684a1dc, {Layout::Tuya, 0x200000,
        0x12A000, 0xA6000,      // download
        0x011000, 0x119000,     // app
        0, 0, true,
        0x1EB000}},             // crash record
    // another build of the Tuya bootloader, same partitions (black relay board with the module upright in a slot,
    // 2026-11-07: app 0x10000+0x108700 and download 0x12A000+0xA6000 in its partition table)
    {0x6802f257, {Layout::Tuya, 0x200000,
        0x12A000, 0xA6000,
        0x011000, 0x119000,
        0, 0, true,
        0x1EB000}},
    // Tuya's bootloader 3.00 of the Matter firmware (MOES MS-104B Matter, 2026-11-07): the app at the same place,
    // but its updates are diff2ya packages (XZ / bspatch, manage sector at 0x1E1000), not our RBL. The Matter code is
    // still written by ESPurna itself; the firmware only over UART (ref. beken/moes-ms-104b-matter.md). No stock dump
    // in firmware-dumps/, its Tuya KV holds the Matter identity: no crash record here
    {0x39afde9f, {Layout::Tuya, 0x200000,
        0x12A000, 0xA6000,
        0x011000, 0x119000,
        0, 0, false,
        0}},
    {0x64b02979, {Layout::Quectel, 0x400000,
        0x286000, 0x15A000,
        0x011000, 0x1A9000,
        0x3E0000, 0x020000,     // Quectel Matter factory data ("chip-factory")
        true,
        0x1EB000}},
};

using ::espurna::crc32_ieee::crc32Update;

FlashLayout detect() {
    FlashLayout out {};
    out.flash_size = lt_flash_get_size();

    uint8_t buffer[256];
    uint32_t crc { 0 };
    for (uint32_t offset = 0; offset < BootloaderSize; offset += sizeof(buffer)) {
        if (lt_flash_read(offset, buffer, sizeof(buffer)) != sizeof(buffer)) {
            return out;
        }
        crc = crc32Update(crc, buffer, sizeof(buffer));
    }

    for (const auto& known : KnownLayouts) {
        if ((known.crc == crc) && (known.layout.flash_size <= out.flash_size)) {
            return known.layout;
        }
    }

    DEBUG_MSG_P(PSTR("[MAIN] Unknown bootloader (CRC32 %08X), flash %u KiB: no OTA\n"),
        crc, out.flash_size / 1024);
    return out;
}

} // namespace

const FlashLayout& flashLayout() {
    static const FlashLayout layout = detect();
    return layout;
}

const char* layoutName(Layout value) {
    switch (value) {
    case Layout::Tuya:
        return "Tuya";
    case Layout::Quectel:
        return "Quectel";
    case Layout::Unknown:
        break;
    }

    return "unknown";
}

} // namespace beken
} // namespace espurna

