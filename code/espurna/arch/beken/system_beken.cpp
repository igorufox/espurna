/*

SYSTEM MODULE FOR BEKEN (BDK)

Chip part, the FreeRTOS chips part is in arch/rtos/system_rtos.cpp, common part is in system.cpp

*/

#include <algorithm>
#include <cstring>


#include "../../espurna.h"
#include "../../rtcmem.h"
#include "../../crash.h"
#include "layout_beken.h"
#include "beken_api.h"

#include <wifi_sdk.h>

// the loop task (core_beken.cpp app_main()) runs setup() and loop(), remembered by the first pre_setup() call.
// Same name as the ESP32 Arduino core one, debug.cpp uses it to tell the loop task apart
TaskHandle_t loopTaskHandle { nullptr };

namespace {

// hardware WDT, fed by chip::loop_pass() after every loop()
constexpr uint32_t LoopWdtTimeout { 10000 };

} // namespace

// -----------------------------------------------------------------------------
// MEMORY
// -----------------------------------------------------------------------------

size_t systemFreeHeap() {
    return ::espurna::beken::heap_free();
}

size_t systemHeapMinimum() {
    return ::espurna::beken::heap_min_free();
}

// .tcm output section of bk7231n_bsp.ld, which starts at the beginning of the 'tcm' region (0x003F0000,
// 60 KiB - 512 in bdk/bk7231n_bsp.ld). Weak: other chips of the family have no such section
extern "C" uint8_t _tcmbss_end[] __attribute__((weak));

TcmStats systemTcmStats() {
    constexpr uintptr_t Start { 0x003F0000 };
    constexpr size_t Size { 60 * 1024 - 512 };

    const auto end = reinterpret_cast<uintptr_t>(_tcmbss_end);
    if (!end || (end < Start) || (end > (Start + Size))) {
        return {0, 0};
    }

    return {end - Start, Size};
}

// systemHeapStats(), arch/rtos/system_rtos.cpp. FreeRTOS v9 of the BK72xx SDK does not track the largest free block,
// heap_max_alloc() is 0 then
void espurna::system::chip::sdk::heap(uint32_t& free, uint32_t& block) {
    free = ::espurna::beken::heap_free();
    block = ::espurna::beken::heap_max_alloc();
}

// -----------------------------------------------------------------------------
// MAC
// -----------------------------------------------------------------------------

// libcal, reads the MAC out of the RF calibration block (the 'calibration' partition of the board)
extern "C" int manual_cal_get_macaddr_from_flash(uint8_t* mac);

namespace {
namespace mac {

// The SDK reads the MAC from the RF calibration block. Boards that have no such block (no Tuya config, e.g. the Sparkleiot
// XH-CB2S) all fall back to the same C8:47:8C:42:00:48, and two of them on one network get one IP. Such a board gets its
// own locally administered address once (CA:47:8C + 3 random bytes), in a block of its own. The SDK cannot add one, its
// manual_cal_write_macaddr_to_flash() only updates the MAC of a block that is already there
enum class Result {
    Stored,
    Generated,
    Replaced,
    Failed,
};

Result result { Result::Stored };
uint8_t generated[6] {};

// Same layout as a Tuya CB3S / CBU block: "TLV\0" and the length of the rest, then groups of items, each one a tag,
// a length and the data. MAC is the item 0x11111102 of the chip info group 0x11111100
struct __attribute__((packed)) Block {
    uint32_t magic;
    uint32_t length;
    uint32_t group;
    uint32_t group_length;
    uint32_t item;
    uint32_t item_length;
    uint8_t mac[6];
};

constexpr uint32_t BlockMagic { 0x00564c54 };
constexpr uint32_t ChipInfoGroup { 0x11111100 };
constexpr uint32_t MacItem { 0x11111102 };

// The first build took the random bytes from rand(), which nothing seeds, and every board got this one
constexpr uint8_t Unseeded[6] { 0xca, 0x47, 0x8c, 0x2d, 0xf4, 0x51 };

Block make_block(const uint8_t* mac) {
    Block out {
        .magic = BlockMagic,
        .length = sizeof(Block) - 8,
        .group = ChipInfoGroup,
        .group_length = sizeof(Block) - 16,
        .item = MacItem,
        .item_length = sizeof(Block::mac),
        .mac = {},
    };
    std::memcpy(out.mac, mac, sizeof(out.mac));

    return out;
}

// the sector holds nothing but this block (or nothing at all, without one)
bool sector_holds(const Block* block) {
    uint8_t chunk[256];
    for (uint32_t offset = 0; offset < FLASH_CALIBRATION_LENGTH; offset += sizeof(chunk)) {
        if (::espurna::beken::flash_read(FLASH_CALIBRATION_OFFSET + offset, chunk, sizeof(chunk)) != sizeof(chunk)) {
            return false;
        }

        size_t from = 0;
        if (block && (offset == 0)) {
            if (std::memcmp(chunk, block, sizeof(Block)) != 0) {
                return false;
            }

            from = sizeof(Block);
        }

        if (std::any_of(std::begin(chunk) + from, std::end(chunk), [](uint8_t value) { return value != 0xff; })) {
            return false;
        }
    }

    return true;
}

bool write_block(const uint8_t* mac, bool replace) {
    if (replace) {
        if (!::espurna::beken::flash_erase(FLASH_CALIBRATION_OFFSET, FLASH_CALIBRATION_LENGTH)) {
            return false;
        }
    } else if (!sector_holds(nullptr)) {
        return false;
    }

    const auto block = make_block(mac);
    const auto* data = reinterpret_cast<const uint8_t*>(&block);
    if (::espurna::beken::flash_write(FLASH_CALIBRATION_OFFSET, data, sizeof(block)) != sizeof(block)) {
        return false;
    }

    return sector_holds(&block);
}

// the TRNG of the chip (core_beken.cpp, the entropy of mbedTLS too): rand() of the libc is never seeded otherwise and
// gives every board the same numbers
uint32_t hardware_random() {
    uint32_t out { 0 };
    ::espurna::beken::trng_bytes(reinterpret_cast<uint8_t*>(&out), sizeof(out));
    return out;
}

// Tuya Matter firmware keeps the RF calibration block in another sector than the 'calibration' partition of the board
// (MOES MS-104B Matter with a CBU module, 2026-11-07: 0x1E3000, 0x1D0000 blank). Without it the SDK runs on its
// defaults and the radio is too weak for even the softAP to be seen. The block goes where the SDK reads it, on the first
// boot: our settings pool (storage_flash_beken.cpp, 0x1DB000..0x1EB000) covers that sector and erase it later on
constexpr uint32_t OtherCalibrationOffsets[] { 0x1E3000 };

bool is_ours(const uint8_t* header) {
    Block block;
    std::memcpy(&block, header, sizeof(block));
    return (block.magic == BlockMagic)
        && (block.length == (sizeof(Block) - 8))
        && (block.group == ChipInfoGroup)
        && (block.item == MacItem);
}

// a whole block of the vendor, not only a MAC like ours
bool is_vendor_block(uint32_t offset) {
    uint32_t header[2];
    if (::espurna::beken::flash_read(offset, reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
        return false;
    }

    return (header[0] == BlockMagic)
        && (header[1] > (sizeof(Block) - 8))
        && (header[1] <= (FLASH_CALIBRATION_LENGTH - 8));
}

bool copy_calibration(uint32_t from) {
    if (!::espurna::beken::flash_erase(FLASH_CALIBRATION_OFFSET, FLASH_CALIBRATION_LENGTH)) {
        return false;
    }

    // the header goes last, a copy cut short by a power loss is not taken for a block and is made again
    uint8_t chunk[256];
    for (uint32_t offset = 0; offset < FLASH_CALIBRATION_LENGTH; offset += sizeof(chunk)) {
        if (::espurna::beken::flash_read(from + offset, chunk, sizeof(chunk)) != sizeof(chunk)) {
            return false;
        }

        const size_t skip = (offset == 0) ? 8 : 0;
        if (::espurna::beken::flash_write(FLASH_CALIBRATION_OFFSET + offset + skip, chunk + skip, sizeof(chunk) - skip)
                != (sizeof(chunk) - skip)) {
            return false;
        }
    }

    if (::espurna::beken::flash_read(from, chunk, 8) != 8) {
        return false;
    }

    if (::espurna::beken::flash_write(FLASH_CALIBRATION_OFFSET, chunk, 8) != 8) {
        return false;
    }

    uint8_t copied[256];
    for (uint32_t offset = 0; offset < FLASH_CALIBRATION_LENGTH; offset += sizeof(chunk)) {
        if ((::espurna::beken::flash_read(from + offset, chunk, sizeof(chunk)) != sizeof(chunk))
            || (::espurna::beken::flash_read(FLASH_CALIBRATION_OFFSET + offset, copied, sizeof(copied)) != sizeof(copied))
            || (std::memcmp(chunk, copied, sizeof(chunk)) != 0))
        {
            return false;
        }
    }

    return true;
}

enum class Calibration {
    Unchecked,
    InPlace,
    Copied,
    Failed,
};

Calibration calibration { Calibration::Unchecked };
uint32_t calibration_from { 0 };

// before the SDK reads the block (the first WiFi.mode() of pre_setup()) and before the MAC below
void ensure_calibration() {
    calibration = Calibration::InPlace;
    if (::espurna::beken::flashLayout().layout != ::espurna::beken::Layout::Tuya) {
        return;
    }

    uint8_t header[sizeof(Block)];
    if (::espurna::beken::flash_read(FLASH_CALIBRATION_OFFSET, header, sizeof(header)) != sizeof(header)) {
        calibration = Calibration::Failed;
        return;
    }

    // a block of the vendor already is where the SDK looks, or a MAC of ours over nothing at all
    uint32_t magic;
    std::memcpy(&magic, header, sizeof(magic));
    if ((magic == BlockMagic) && !is_ours(header)) {
        return;
    }

    for (const auto offset : OtherCalibrationOffsets) {
        if ((offset + FLASH_CALIBRATION_LENGTH) > ::espurna::beken::flash_size()) {
            continue;
        }

        if (is_vendor_block(offset)) {
            calibration_from = offset;
            calibration = copy_calibration(offset)
                ? Calibration::Copied
                : Calibration::Failed;
            return;
        }
    }
}

void report_calibration() {
    switch (calibration) {
    case Calibration::Unchecked:
    case Calibration::InPlace:
        break;
    case Calibration::Copied:
        DEBUG_MSG_P(PSTR("[MAIN] RF calibration block copied from 0x%06X to 0x%06X\n"),
            calibration_from, FLASH_CALIBRATION_OFFSET);
        break;
    case Calibration::Failed:
        DEBUG_MSG_P(PSTR("[MAIN] RF calibration block at 0x%06X could not be copied to 0x%06X\n"),
            calibration_from, FLASH_CALIBRATION_OFFSET);
        break;
    }
}

// once, from whichever comes first: the chip id (the boot log asks for it before system setup) or pre_setup()
void ensure_unique() {
    static bool done { false };
    if (done) {
        return;
    }

    done = true;

    ensure_calibration();

    // ::espurna::beken::rand_bytes() and our RandomDevice are rand() as well
    srand(hardware_random());

    bool replace { false };

    uint8_t stored[6];
    if (manual_cal_get_macaddr_from_flash(stored)) {
        if (std::memcmp(stored, Unseeded, sizeof(stored)) != 0) {
            return;
        }

        // only our own block, a Tuya one with this MAC (not likely) stays as it is
        const auto ours = make_block(Unseeded);
        if (!sector_holds(&ours)) {
            return;
        }

        replace = true;
    }

    const auto random = hardware_random();
    generated[0] = 0xca;
    generated[1] = 0x47;
    generated[2] = 0x8c;
    generated[3] = (random >> 16) & 0xff;
    generated[4] = (random >> 8) & 0xff;
    generated[5] = random & 0xff;

    // not written means the default stays, a new random one on every boot would be worse
    result = write_block(generated, replace)
        ? (replace ? Result::Replaced : Result::Generated)
        : Result::Failed;
}

// the SDK reads the block once, the new MAC is used from the next boot on and set by hand for this one
void apply() {
    if ((result == Result::Generated) || (result == Result::Replaced)) {
        ::espurna::core::chip::wifi_set_mac(false, generated);
    }
}

void report() {
    report_calibration();

    switch (result) {
    case Result::Stored:
        break;
    case Result::Generated:
        DEBUG_MSG_P(PSTR("[MAIN] No RF calibration block, MAC %s written to a new one\n"),
            wifiStaMac().c_str());
        break;
    case Result::Replaced:
        DEBUG_MSG_P(PSTR("[MAIN] MAC of the first build was the same on every board, %s written instead\n"),
            wifiStaMac().c_str());
        break;
    case Result::Failed:
        DEBUG_MSG_P(PSTR("[MAIN] No MAC in the RF calibration block and none could be written, using the SDK default\n"));
        break;
    }
}

} // namespace mac
} // namespace

// the settings pool (storage_flash_beken.cpp) covers the other calibration sectors: until the block is known to be
// where the SDK reads it, none of them is erased. Tells the storage which flash sector to keep, 0 - none
uint32_t bekenCalibrationKeep() {
    if (mac::calibration == mac::Calibration::Unchecked) {
        mac::ensure_unique();
    }

    if (mac::calibration != mac::Calibration::Failed) {
        return 0;
    }

    return mac::calibration_from ? mac::calibration_from : mac::OtherCalibrationOffsets[0];
}

uint32_t espurna::system::RandomDevice::operator()() const {
    uint32_t out;
    ::espurna::beken::rand_bytes(reinterpret_cast<uint8_t*>(&out), sizeof(out));
    return out;
}

// -----------------------------------------------------------------------------
// SDK CONFIG
// -----------------------------------------------------------------------------

// esp8266 erases the SDK area, i.e. stored WiFi config and RF calibration.
// the BDK does not store the Wi-Fi config here, the RF calibration is kept
bool eraseSDKConfig() {
    return true;
}

namespace espurna {
namespace system {

// One core, masking its interrupts is enough. Nests
uint32_t criticalEnter() {
    return static_cast<uint32_t>(port_disable_interrupts_flag());
}

void criticalExit(uint32_t state) {
    port_enable_interrupts_flag(static_cast<int>(state));
}

namespace chip {

// yield() only feeds it between loop() calls
void sdk::feed_loop_wdt() {
    ::espurna::beken::wdt_feed();
}

[[noreturn]] void sdk::reboot() {
    // bk_reboot() writes the reboot type into the retention register. Same reboot,
    // but with the system word (custom reset reason) there instead (ref. rtcmem_beken.cpp)
    constexpr uintptr_t RegRetention { 0x00800150 };

    portDISABLE_INTERRUPTS();
    *reinterpret_cast<volatile uint32_t*>(RegRetention) = rtcmemRetention();

    ::espurna::beken::wdt_disable();
    ::espurna::beken::wdt_enable(5);
    for (;;) {
    }
}

// the SDK one, or the one we made when the RF calibration block had none (ref. mac::ensure_unique())
bool mac(uint8_t* out) {
    ::mac::ensure_unique();

    if ((::mac::result == ::mac::Result::Generated) || (::mac::result == ::mac::Result::Replaced)) {
        std::memcpy(out, ::mac::generated, 6);
    } else {
        ::espurna::beken::device_mac(out);
    }

    return true;
}

// Not implemented yet, systemBeforeSleep() / systemAfterSleep() callbacks are kept for the API
bool light_sleep(bool, sleep::Microseconds, int, sleep::Interrupt) {
    return false;
}

bool deep_sleep(sleep::Microseconds) {
    return false;
}

void early_setup() {
}

// macSet = aa:bb:cc:dd:ee:ff: another MAC in the RF calibration block (the SDK updates the MAC item of the block that is
// there, the calibration stays), used from the next boot on; the key is removed once written. A bench tool: a controller
// that knows a board by its MAC sees a new one (2026-11-16)
extern "C" int manual_cal_write_macaddr_to_flash(uint8_t* mac);

void mac_set() {
    if (!hasSetting("macSet")) {
        return;
    }

    espurna::string text = getSetting("macSet");
    delSetting("macSet");
    text.replace(":", "");
    text.replace("-", "");

    uint8_t out[6];
    uint8_t stored[6];
    if ((text.length() != 12) || (hexDecode(text.c_str(), text.length(), out, sizeof(out)) != sizeof(out))
        || (out[0] & 0x01) || !manual_cal_get_macaddr_from_flash(stored))
    {
        DEBUG_MSG_P(PSTR("[MAIN] macSet: not a unicast MAC or no RF calibration block\n"));
        return;
    }

    manual_cal_write_macaddr_to_flash(out);
    const bool written = manual_cal_get_macaddr_from_flash(stored) && (std::memcmp(stored, out, sizeof(out)) == 0);
    DEBUG_MSG_P(PSTR("[MAIN] macSet: %02X:%02X:%02X:%02X:%02X:%02X %s\n"),
        out[0], out[1], out[2], out[3], out[4], out[5], written ? "written, used from the next boot on" : "NOT written");
}

void sdk::setup() {
    ::mac::report();
    mac_set();
}

// hardware WDT, fed by chip::loop_pass() after every loop() (core_beken.cpp). The stall watcher learns the period from
// beken::wdt_* (rtcmem_beken.cpp)
void sdk::enable_loop_wdt() {
    ::espurna::beken::wdt_enable(LoopWdtTimeout);
}

} // namespace chip

namespace chip {

void pre_setup() {
    loopTaskHandle = xTaskGetCurrentTaskHandle();

    // The TCP servers (libs/tcp.h: web, telnet) are started before WiFi is, make sure the TCP/IP stack is up.
    // the BDK's apps (the lwip tcpip thread included) start with the first chip::wifi_mode() (core_beken.cpp)
    // change, until then any tcpip_api_call() waits forever. Station is not connected here,
    // WiFi module sets the mode it needs later on
    ::espurna::core::chip::wifi_mode(WIFI_MODE_STA);
    ::espurna::core::chip::wifi_mode(WIFI_MODE_NULL);

    // the chip id is made of the MAC, which has to be unique by now
    mac::ensure_unique();
    mac::apply();
}

// Same values as esp8266, common code (e.g. the stability check) relies on them
uint32_t reset_reason() {
    // SDK sees the system word written by restart() or our crash handlers as an unknown start type,
    // i.e. a watchdog reset
    if (rtcmemSysStatus()) {
        switch (rtcmemCrash()) {
        case RtcmemCrash::None:
            return REASON_SOFT_RESTART;
        case RtcmemCrash::Stall:
            return REASON_WDT_RST;
        default:
            return REASON_EXCEPTION_RST;
        }
    }

    using ::espurna::beken::Reboot;
    switch (::espurna::beken::reboot_reason()) {
    case Reboot::Software:
        return REASON_SOFT_RESTART;
    case Reboot::Crash:
        return REASON_EXCEPTION_RST;
    case Reboot::Watchdog:
        return REASON_WDT_RST;
    case Reboot::SleepGpio:
    case Reboot::SleepRtc:
    case Reboot::SleepUsb:
        return REASON_DEEP_SLEEP_AWAKE;
    // power related, not the firmware's fault
    default:
        break;
    }

    return REASON_DEFAULT_RST;
}

} // namespace chip
} // namespace system

// Crash record is kept in a flash record of its own, written by the trap handlers (ref. rtcmem_beken.cpp).
// The EEPROM part is not written here
namespace debug {
namespace crash {
namespace chip {

bool dump(::espurna::Output& print, bool check) {
    if (!check) {
        rtcmemCrashDump(print);
    }

    return true;
}

bool dump_empty(::espurna::Output&) {
    return false;
}

bool cause(CrashCause& out) {
    rtcmemCrashCause(out);
    return true;
}

void setup() {
    rtcmemCrashSetup();
}

} // namespace chip
} // namespace crash
} // namespace debug
} // namespace espurna

