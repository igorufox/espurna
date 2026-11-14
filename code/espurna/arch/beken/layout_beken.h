/*

BK7231N FLASH LAYOUT, PICKED AT BOOT

*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace beken {

// Which bootloader the board has: it decides where an OTA image goes and how much of the image fits there.
// Same firmware on both, ref. scripts/beken_image.py
enum class Layout {
    Unknown,
    Tuya,    // Tuya BK7231N bootloader, 2 MiB: CB2S, CB3S, CBU ... (flash encryption key of Tuya)
    Quectel, // Quectel bootloader, 4 MiB, no flash encryption: XH-CB2S / BK7231M (Sparkleiot Mini)
};

struct FlashLayout {
    Layout layout;
    uint32_t flash_size;
    // where the bootloader picks an OTA image (RBL) up from
    uint32_t download_offset;
    uint32_t download_length;
    // the application, as the bootloader installs it (with the CRC of every 32 bytes)
    uint32_t app_offset;
    uint32_t app_length;
    // Matter factory data of the stock firmware, 0 when there is none
    uint32_t matter_offset;
    uint32_t matter_length;
    // the bootloader installs our OTA image (RBL). Tuya's bootloader 3.00 of the Matter firmware takes only its own
    // diff2ya packages: firmware updates over UART there, or after its replacement by the standard Tuya one
    bool installs_rbl { true };
    // the sector of the crash record (rtcmem_beken.cpp), 0 - none: only where the stock dumps of that bootloader
    // show it blank (firmware-dumps/: CB3S, CBU, the slot board, the square QFN, two XH-CB2S - 0x1EB000 blank)
    uint32_t crash_offset { 0 };
    // what the bootloader / the stock firmware keeps between the app and the end of 'download' (never modules there):
    // the sectors the stock dumps of that bootloader show written (firmware-dumps/, two XH-CB2S: 0x210000, 0x220000)
    struct Range {
        uint32_t offset;
        uint32_t size;
    };
    Range reserved[2] {};
};

const FlashLayout& flashLayout();
const char* layoutName(Layout);

} // namespace beken
} // namespace espurna
