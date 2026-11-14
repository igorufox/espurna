/*

PROTOTYPE: what the BK7231N gives the module loader (modules_core.h; the loader is modules.cpp; docs/modules.md,
"Beken")

Flash: the modules lie in the bootloader's 'download' partition (the OTA image goes there only while the head updates
itself, the modules are erased for that, as on the ESP8266). Its first sector stays free (the RBL header the bootloader
looks at), its last sector is the directory (raw bytes). Modules are placed from the top down, in whole sectors.

Code runs from the flash through the mapping of the flash controller (XIP): logical address L is stored at flash
offset L / 32 * 34 as 32 bytes encrypted with the chip's key, then their CRC-16 (big endian). The board writes a
module in that format itself: the words are encrypted with the key found at boot (compared against its own app), the
CRC of every 32 bytes follows them. A module starts on a 32 byte boundary of the logical space, inside its sectors
(Entry::bases[BaseFlash]: that logical address). Every BL / BLX from it into the head is moved by the flash delta (the
branch list of the package). The fixed pool: the data TCM the head leaves free (from _tcmbss_end, the TCM is not heap).

*/

#include "espurna.h"
#include "modules.h"

#if MODULES_SUPPORT

#include "modules_core.h"
#include "terminal.h"

#include "layout_beken.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

// heap the head keeps for itself (Wi-Fi, the web server) when the Apply sizes the modules' heap block
#ifndef MODULES_HEAP_KEEP
#define MODULES_HEAP_KEEP 8192
#endif

extern "C" {
// the data TCM: the head's .bss there ends here (the SDK's linker script), the TCM ends at the other (scripts/beken_tcm.py)
extern uint8_t _tcmbss_end[];
extern uint8_t _espurna_tcm_end[];

}

namespace espurna {
namespace modules {
namespace {

constexpr uint32_t BlockData { 32 };
constexpr uint32_t BlockSize { 34 };

// branch list entries: offset in the chunk | kind << 16
constexpr uint32_t BranchThumb { 1 };   // Thumb BL / BLX pair (two half words)
constexpr uint32_t BranchArm { 2 };     // ARM BL / BLX

// the fixed pool: the free data TCM
uint32_t tcmStart() {
    return roundUp(reinterpret_cast<uint32_t>(_tcmbss_end), 8);
}

uint32_t tcmSize() {
    const uint32_t end = reinterpret_cast<uint32_t>(_espurna_tcm_end) & ~7u;
    return (end > tcmStart()) ? (end - tcmStart()) : 0;
}

// --- the flash format ----------------------------------------------------------------------------

uint32_t physical(uint32_t logical) {
    return logical / BlockData * BlockSize;
}

// first logical block boundary at or after a flash offset
uint32_t logicalFrom(uint32_t offset) {
    return (offset + BlockSize - 1) / BlockSize * BlockData;
}

uint16_t crc16(const uint8_t* data, size_t length) {
    uint16_t crc { 0xffff };
    for (size_t index = 0; index < length; ++index) {
        crc ^= static_cast<uint16_t>(data[index]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000)
                ? static_cast<uint16_t>((crc << 1) ^ 0x8005)
                : static_cast<uint16_t>(crc << 1);
        }
    }

    return crc;
}

// The address scrambling of the BK72xx flash controller (encrypt_crc/abc.c of the SDK, ltchiptool's BekenCrypto):
// every 32 bit word is XORed with a pad made from its logical address and the four key words
struct Crypt {
    bool bypass { true };
    bool use15 { false };
    bool use16 { false };
    bool use32 { false };
    uint8_t sel15 { 0 };
    uint8_t sel16 { 0 };
    uint8_t sel32 { 0 };
    uint32_t coef0 { 0 };
    uint32_t coef1_hi16 { 0 };
    uint32_t coef1_mix { 0 };
    uint32_t random { 0 };

    static uint32_t bits(uint32_t value, int hi, int lo) {
        return (value >> lo) & ((hi - lo == 31) ? 0xffffffffu : ((1u << (hi - lo + 1)) - 1));
    }

    void init(const uint32_t coef[4]) {
        *this = Crypt{};
        const uint32_t coef3 = coef[3];
        bypass = (bits(coef3, 31, 24) == 0x00) || (bits(coef3, 31, 24) == 0xff) || (bits(coef3, 3, 0) == 0xf);
        if (bypass) {
            return;
        }

        coef0 = coef[0];
        coef1_hi16 = bits(coef[1], 31, 16);
        coef1_mix = (bits(coef[1], 15, 8) << 9) | (bits(coef3, 4, 4) << 8) | bits(coef[1], 7, 0);
        use15 = !bits(coef3, 0, 0);
        use16 = !bits(coef3, 1, 1);
        use32 = !bits(coef3, 2, 2);
        random = bits(coef3, 3, 3) ? 0 : coef[2];
        sel15 = bits(coef3, 6, 5);
        sel16 = bits(coef3, 9, 8);
        sel32 = bits(coef3, 12, 11);
    }

    static uint32_t pn15(uint32_t addr) {
        const uint32_t a = (bits(addr, 6, 0) << 9) | bits(addr, 15, 7);
        const uint32_t c = 0x6371 & (bits(addr, 8, 5) * 0x1111);
        return (a ^ c) & 0xffff;
    }

    static uint32_t pn16(uint32_t addr) {
        const uint32_t a = (bits(addr, 9, 0) << 7) | bits(addr, 16, 10);
        const uint32_t b = bits(addr, 13, 13) | (bits(addr, 9, 9) << 1) | (bits(addr, 5, 5) << 2) | (bits(addr, 1, 1) << 3);
        const uint32_t d = 0x13659 & ((bits(addr, 4, 4) * 0x10000) + (b * 0x1111));
        return (a ^ d) & 0x1ffff;
    }

    static uint32_t pn32(uint32_t addr) {
        const uint32_t a = (bits(addr, 14, 0) << 17) | bits(addr, 31, 15);
        const uint32_t c = 0xE519A4F1u & (bits(addr, 5, 2) * 0x11111111u);
        return a ^ c;
    }

    uint32_t pad(uint32_t addr) const {
        if (bypass) {
            return 0;
        }

        uint32_t v15 { 0 };
        if (use15) {
            const uint32_t b3 = bits(addr, 31, 24);
            const uint32_t b2 = bits(addr, 23, 16);
            const uint32_t b1 = bits(addr, 15, 8);
            const uint32_t b0 = bits(addr, 7, 0);
            const uint32_t hi = (sel15 & 2) ? ((b2 << 8) | b3) : ((b3 << 8) | b2);
            const uint32_t lo = (sel15 & 1) ? ((b0 << 8) | b1) : ((b1 << 8) | b0);
            v15 = pn15((hi ^ lo) ^ coef1_hi16);
        }

        uint32_t v16 { 0 };
        if (use16) {
            v16 = pn16(bits(addr, 16 + sel16, sel16) ^ coef1_mix);
        }

        uint32_t v32 { 0 };
        if (use32) {
            uint32_t a = addr;
            if (sel32) {
                const int shift = 8 * sel32;
                a = (addr >> shift) | (addr << (32 - shift));
            }
            v32 = pn32(a ^ coef0);
        }

        return v32 ^ ((v15 << 16) | (v16 & 0xffff)) ^ random;
    }
};

Crypt crypt;
const char* key_name { "not found" };
bool key_found { false };

struct Key {
    const char* name;
    uint32_t coef[4];
};

constexpr Key Keys[] {
    {"tuya", {0x510fb093, 0xa3cbeadc, 0x5993a17e, 0xc7adeb03}},
    {"none", {0, 0, 0, 0}},
};

// a block of the running app: raw bytes against what the mapping reads. Only a block with a good CRC is read
// through the mapping (a bad one stalls the chip)
bool findKey() {
    constexpr uint32_t Probe { 0x10000 + 0x2000 };
    uint8_t raw[BlockSize * 2];
    if (lt_flash_read(physical(Probe), raw, sizeof(raw)) != sizeof(raw)) {
        return false;
    }

    for (size_t block = 0; block < sizeof(raw); block += BlockSize) {
        const uint16_t stored = (static_cast<uint16_t>(raw[block + 32]) << 8) | raw[block + 33];
        if (crc16(&raw[block], BlockData) != stored) {
            return false;
        }
    }

    for (const auto& key : Keys) {
        Crypt candidate;
        candidate.init(key.coef);
        bool match = true;
        for (size_t index = 0; match && (index < (BlockData * 2 / 4)); ++index) {
            const uint32_t address = Probe + index * 4;
            const uint32_t plain = *reinterpret_cast<const volatile uint32_t*>(address);
            const size_t at = (index / 8) * BlockSize + (index % 8) * 4;
            uint32_t stored;
            std::memcpy(&stored, &raw[at], 4);
            match = ((plain ^ candidate.pad(address)) == stored);
        }

        if (match) {
            crypt = candidate;
            key_name = key.name;
            key_found = true;
            return true;
        }
    }

    return false;
}

// 32 byte blocks of plain data at a logical address (32 aligned) into the flash: encrypted, CRC'd. The sectors
// must be erased already
bool writeBlocks(uint32_t logical, const uint8_t* plain, size_t blocks) {
    constexpr size_t Batch { 8 };
    uint8_t out[BlockSize * Batch];
    while (blocks) {
        const size_t count = std::min(blocks, Batch);
        for (size_t block = 0; block < count; ++block) {
            uint8_t* raw = &out[block * BlockSize];
            for (size_t word = 0; word < (BlockData / 4); ++word) {
                const uint32_t address = logical + (block * BlockData) + (word * 4);
                uint32_t value;
                std::memcpy(&value, &plain[block * BlockData + word * 4], 4);
                value ^= crypt.pad(address);
                std::memcpy(&raw[word * 4], &value, 4);
            }
            const uint16_t crc = crc16(raw, BlockData);
            raw[BlockData] = crc >> 8;
            raw[BlockData + 1] = crc & 0xff;
        }

        const uint32_t size = count * BlockSize;
        if (lt_flash_write(physical(logical), out, size) != size) {
            return false;
        }

        logical += count * BlockData;
        plain += count * BlockData;
        blocks -= count;
    }

    return true;
}

// The flash mapping keeps what it read in a cache (16-32 KiB, measured with MODULES.XIPTEST: after rewriting a block
// the old bytes stay until 32 KiB elsewhere were read through the mapping; nothing in the BDK flushes it). Reading
// 64 KiB of the app pushes every line of written flash out
uint32_t evictCache() {
    uint32_t sink { 0 };
    const auto* ptr = reinterpret_cast<const volatile uint32_t*>(0x10000);
    for (uint32_t index = 0; index < (0x10000 / 4); index += 8) {
        sink += ptr[index];
    }
    return sink;
}

// every block of a logical range checks out (raw reads): safe to read through the mapping
bool blocksValid(uint32_t logical, uint32_t size) {
    uint8_t raw[BlockSize * 8];
    const uint32_t blocks = (size + BlockData - 1) / BlockData;
    for (uint32_t done = 0; done < blocks;) {
        const uint32_t count = std::min<uint32_t>(blocks - done, 8);
        const uint32_t length = count * BlockSize;
        if (lt_flash_read(physical(logical) + done * BlockSize, raw, length) != length) {
            return false;
        }
        for (uint32_t block = 0; block < count; ++block) {
            const uint8_t* ptr = &raw[block * BlockSize];
            const uint16_t stored = (static_cast<uint16_t>(ptr[32]) << 8) | ptr[33];
            if (crc16(ptr, BlockData) != stored) {
                return false;
            }
        }
        done += count;
    }

    return true;
}

// --- the area -------------------------------------------------------------------------------------

struct Region {
    uint32_t start { 0 };       // where the head starts (the meters: lo - start is the head)
    uint32_t lo { 0 };          // first sector for modules
    uint32_t hi { 0 };          // the directory sector
    uint32_t download { 0 };    // the download partition: its first sector holds the RBL of an OTA image
    uint32_t initial { 0 };     // the modules file of the initial image (module_initial_image.py)
    arch::Area::Range keep[arch::Area::KeepMax] {};
    bool valid { false };
};

Region region;

// ESPurna's own sectors of every Beken layout, at fixed offsets: the RF calibration (0x1D0000), Matter's 'kvs'
// (0x1D3000), the settings pool (0x1DB000, storage_flash_beken.cpp), the crash record (0x1EB000, rtcmem_beken.cpp),
// the Tuya config (0x1ED000)
constexpr uint32_t SystemOffset { 0x1D0000 };
constexpr uint32_t SystemEnd { 0x1F0000 };

void keepRange(size_t& count, uint32_t offset, uint32_t size) {
    if (size && (offset < region.hi) && ((offset + size) > region.lo) && (count < arch::Area::KeepMax)) {
        region.keep[count++] = arch::Area::Range{offset, size};
    }
}

// All the flash the layout leaves free, for the modules (owner 2026-11-14: «используем весь предоставленный объём,
// вдруг я все модули захочу включить»): from right behind the head in the app partition (the head takes ~750 KB of
// it) to the end of 'download' (its last sector the directory). The head as linked ends at its logical size, stored as
// 34 bytes per 32 from the app partition on; a sector of room after it. Out of it:
// - the last sector of the app partition (its RBL header, the last 102 bytes: ltchiptool bk72xx binary.py) and the
//   first one of 'download' (the RBL of an OTA image)
// - ESPurna's own sectors (SystemOffset .. SystemEnd) where they lie in between (the Quectel layout: the app ends at
//   0x1BA000, 'download' starts at 0x286000)
// - what the bootloader keeps there (FlashLayout::reserved)
// A head update goes through 'download' and the bootloader writes the app partition: the modules are off for it and
// the Apply writes them anew, as always (W23 2026-11-14: 676 KB -> 1.83 MB on the Tuya layout)
bool findArea() {
    const auto& layout = ::espurna::beken::flashLayout();
    if ((layout.layout == ::espurna::beken::Layout::Unknown) || (layout.download_length < (SectorSize * 4))) {
        return false;
    }

    region = Region{};
    region.download = layout.download_offset;
    region.initial = layout.download_offset + SectorSize;
    region.hi = layout.download_offset + layout.download_length - SectorSize;

    constexpr uint32_t Origin { 0x10000 };
    const uint32_t logical = ESP.getSketchSize() + Origin;
    const uint32_t stored = layout.app_offset + ((logical - Origin + BlockData - 1) / BlockData * BlockSize);
    const uint32_t behind = roundUp(stored, SectorSize) + SectorSize;
    const uint32_t app_end = layout.app_offset + layout.app_length;

    if ((behind + SectorSize) < app_end) {
        region.start = layout.app_offset;
        region.lo = behind;
    } else {
        // no room behind the head: 'download' alone
        region.start = layout.download_offset;
        region.lo = layout.download_offset + SectorSize;
    }

    size_t count { 0 };
    keepRange(count, app_end - SectorSize, SectorSize);
    keepRange(count, layout.download_offset, SectorSize);
    keepRange(count, SystemOffset, SystemEnd - SystemOffset);
    for (const auto& range : layout.reserved) {
        keepRange(count, range.offset, range.size);
    }

    region.valid = region.hi > region.lo;
    return region.valid;
}

void commandsSetup();

// --- the image written: a whole chunk at a time (relocated by the core), the branches moved, encrypted ------------

// one image at a time (the Apply writes the packages one after another)
struct Writing {
    uint32_t slot { 0 };
    uint32_t end { 0 };             // of the image's flash
    uint32_t erased { 0 };          // flash erased up to here
    uint8_t carry[BlockData];       // the last block of the previous chunk, while a branch spans into this one
    uint32_t carry_address { 0 };
    uint32_t carry_delta { 0 };     // what that branch moves by
    bool carry_pending { false };
};

Writing writing;

bool eraseTo(uint32_t end) {
    while (writing.erased < end) {
        if (!flashErase(writing.erased, SectorSize)) {
            return false;
        }
        writing.erased += SectorSize;
    }

    return true;
}

bool writePlain(uint32_t address, const uint8_t* data, size_t size) {
    const uint32_t end = physical(address) + (size / BlockData * BlockSize);
    if ((end > writing.end) || !eraseTo(roundUp(end, SectorSize))) {
        return false;
    }

    return writeBlocks(address, data, size / BlockData);
}

uint16_t half(const uint8_t* ptr) {
    return static_cast<uint16_t>(ptr[0] | (ptr[1] << 8));
}

void setHalf(uint8_t* ptr, uint16_t value) {
    ptr[0] = value & 0xff;
    ptr[1] = value >> 8;
}

// BL / BLX into the head, moved by `delta` bytes: the offset shrinks by as much
const char* moveThumb(uint8_t* first, uint8_t* second, uint32_t delta) {
    const uint16_t h1 = half(first);
    const uint16_t h2 = half(second);
    if (((h1 & 0xf800) != 0xf000) || ((h2 & 0xe800) != 0xe800)) {
        return PSTR("branch list: not a Thumb BL / BLX");
    }

    int32_t offset = static_cast<int32_t>(((h1 & 0x7ff) << 12) | ((h2 & 0x7ff) << 1));
    offset = (offset << 9) >> 9;
    offset -= static_cast<int32_t>(delta);
    if ((offset < -(1 << 22)) || (offset >= (1 << 22))) {
        return PSTR("branch out of range");
    }

    setHalf(first, 0xf000 | ((offset >> 12) & 0x7ff));
    setHalf(second, (h2 & 0xf800) | ((offset >> 1) & 0x7ff));
    return nullptr;
}

const char* moveArm(uint8_t* ptr, uint32_t delta) {
    uint32_t insn;
    std::memcpy(&insn, ptr, 4);
    if ((insn & 0x0e000000) != 0x0a000000) {
        return PSTR("branch list: not an ARM BL / BLX");
    }

    int32_t offset = static_cast<int32_t>(insn << 8) >> 8;
    offset -= static_cast<int32_t>(delta) / 4;
    if ((offset < -(1 << 23)) || (offset >= (1 << 23))) {
        return PSTR("branch out of range");
    }

    insn = (insn & 0xff000000) | (static_cast<uint32_t>(offset) & 0x00ffffff);
    std::memcpy(ptr, &insn, 4);
    return nullptr;
}

// the branches of a whole chunk moved by the flash delta, written; a Thumb pair across the chunk's end waits for the
// next chunk (its last block is held back). A branch into a module it needs (LinkBranch) moves by the flash delta of
// this module less the provider's; the words of the links (LinkWord) the core did already
const char* writeChunk(Upload& up, uint32_t offset, uint8_t* chunk, size_t size) {
    const uint32_t address = up.bases[BaseFlash] + offset;
    const uint32_t delta = up.delta[1];

    // a Thumb branch from the end of the previous chunk into this one
    if (writing.carry_pending) {
        const auto* error = moveThumb(&writing.carry[BlockData - 2], &chunk[0], writing.carry_delta);
        if (error) {
            return error;
        }
        if (!writePlain(writing.carry_address, writing.carry, BlockData)) {
            return PSTR("flash write");
        }
        up.image_crc = crc32Update(up.image_crc, writing.carry, BlockData);
        writing.carry_pending = false;
    }

    bool spans = false;
    for (uint32_t index = 0; index < up.branch_count; ++index) {
        const uint32_t at = up.branches[index] & 0xffff;
        uint32_t kind = up.branches[index] >> 16;
        uint32_t move = delta;
        if (isLink(kind)) {
            if ((kind & ~0xfu) != LinkBranch) {
                continue;
            }
            move = delta - up.provider[(kind >> 2) & 3][1];
            kind &= 3;
        }

        const char* error { nullptr };
        if ((kind == BranchThumb) && ((at + 2) == size) && (size == ChunkSize)) {
            spans = true;
            writing.carry_delta = move;
            continue;
        }
        if ((kind == BranchThumb) && ((at + 4) <= size) && !(at & 1)) {
            error = moveThumb(&chunk[at], &chunk[at + 2], move);
        } else if ((kind == BranchArm) && ((at + 4) <= size) && !(at & 3)) {
            error = moveArm(&chunk[at], move);
        } else {
            error = PSTR("branch list: bad entry");
        }
        if (error) {
            return error;
        }
    }

    const uint32_t now = spans ? (size - BlockData) : size;
    if (!writePlain(address, chunk, now)) {
        return PSTR("flash write");
    }
    up.image_crc = crc32Update(up.image_crc, chunk, now);

    if (spans) {
        std::memcpy(writing.carry, &chunk[size - BlockData], BlockData);
        writing.carry_address = address + size - BlockData;
        writing.carry_pending = true;
    }

    return nullptr;
}

// --- terminal: the XIP proof (step 1 of the Beken design) -------------------------------------------------------

// int f(int (*cb)(int), int a) { return cb(a) + K; } in Thumb, K in the literal at +16
constexpr uint8_t TestCode[] {
    0x10, 0xb5,     // push {r4, lr}
    0x03, 0x00,     // movs r3, r0
    0x08, 0x00,     // movs r0, r1
    0x98, 0x47,     // blx r3
    0x01, 0x49,     // ldr r1, [pc, #4]
    0x40, 0x18,     // adds r0, r0, r1
    0x10, 0xbd,     // pop {r4, pc}
    0xc0, 0x46,     // nop
};

int testCallback(int value) {
    return value * 2;
}

#if TERMINAL_SUPPORT
// one block of code in the free sector under the directory's two copies: written, read back raw and mapped, called; then
// written again with another constant at the same address (stale reads?), called again; the sector erased after
void commandXipTest(::terminal::CommandContext&& ctx) {
    if (!region.valid) {
        terminalError(ctx, F("no module area"));
        return;
    }

    if (!key_found) {
        terminalError(ctx, F("no flash key found"));
        return;
    }

    if (modulesRunning()) {
        terminalError(ctx, F("modules are running"));
        return;
    }

    // the last sector is the directory, the one under it its second copy (DirectorySectors)
    const uint32_t sector = region.hi - (DirectorySectors * SectorSize);
    Directory dir;
    if (readDirectory(dir)) {
        for (size_t index = 0; index < dir.count; ++index) {
            const auto& entry = dir.entries[index];
            if ((entry.offset < (sector + SectorSize)) && (sector < (entry.offset + entry.slot))) {
                terminalError(ctx, F("a module takes the test sector (MODULES.REMOVE it first)"));
                return;
            }
        }
    }
    const uint32_t address = logicalFrom(sector);
    ctx.output.printf_P(PSTR("key %s, sector 0x%06x, logical 0x%06x (flash 0x%06x)\n"),
        key_name, sector, address, physical(address));

    uint32_t constants[] { 0x11110100, 0x22220200, 0x33330300 };
    for (size_t round = 0; round < 3; ++round) {
        uint8_t block[BlockData];
        std::memset(block, 0, sizeof(block));
        std::memcpy(block, TestCode, sizeof(TestCode));
        std::memcpy(&block[16], &constants[round], 4);

        const auto t0 = micros();
        if (!flashErase(sector, SectorSize) || !writeBlocks(address, block, 1)) {
            terminalError(ctx, F("flash write"));
            return;
        }
        const auto t1 = micros();

        if (!blocksValid(address, BlockData)) {
            terminalError(ctx, F("raw block CRC"));
            flashErase(sector, SectorSize);
            return;
        }

        const auto* mapped = reinterpret_cast<const volatile uint8_t*>(address);
        auto count = [&]() {
            size_t out = 0;
            for (size_t index = 0; index < BlockData; ++index) {
                out += (mapped[index] == block[index]) ? 1 : 0;
            }
            return out;
        };

        size_t same = count();
        const size_t first = same;

        // stale: how much has to be read through the mapping elsewhere (the app) before the new bytes show
        uint32_t evicted { 0 };
        uint32_t sink { 0 };
        for (uint32_t amount = 16; (same != BlockData) && (amount <= 0x10000); amount *= 2) {
            const auto* other = reinterpret_cast<const volatile uint32_t*>(0x20000 + (round * 0x20000));
            for (uint32_t index = 0; index < (amount / 4); ++index) {
                sink += other[index];
            }
            evicted = amount;
            same = count();
        }

        int result { 0 };
        if (same == BlockData) {
            using Func = int(*)(int(*)(int), int);
            auto func = reinterpret_cast<Func>(address | 1);
            result = func(testCallback, 21);
        }

        ctx.output.printf_P(PSTR("round %u: written in %u us, mapped read %u/%u bytes as written (%u after reading "
            "%u bytes elsewhere, %08x), f(cb, 21) = 0x%08x (expected 0x%08x) %s\n"),
            round + 1, static_cast<unsigned>(t1 - t0), first, BlockData, same, evicted, sink,
            result, 42 + constants[round], (result == static_cast<int>(42 + constants[round])) ? "OK" : "WRONG");
    }

    flashErase(sector, SectorSize);
    terminalOK(ctx);
}

PROGMEM_STRING(XipTest, "MODULES.XIPTEST");

static constexpr ::terminal::Command Commands[] PROGMEM {
    {XipTest, commandXipTest},
};
#endif

void commandsSetup() {
#if TERMINAL_SUPPORT
    espurna::terminal::add(Commands);
#endif
}

} // namespace

// --- what the chip gives the loader (modules_core.h) -----------------------------------------------------------------

namespace arch {

// first in modulesBootSetup(): the area (by the bootloader), the chip's key, the XIP test command
void setup() {
    if (!findArea()) {
        DEBUG_MSG_P(PSTR("[MODULES] No module area (unknown bootloader)\n"));
    } else if (!findKey()) {
        DEBUG_MSG_P(PSTR("[MODULES] The flash key of this chip is unknown, no modules\n"));
    }

    commandsSetup();
}

// an undefined instruction, as module code would crash (a write to an unmapped address does not fault on BK7231N)
void crashNow() {
    __asm__ volatile(".inst.n 0xde00");
}

// Flash = the OTA partition, its first sector shown as the "head"
Area area() {
    // modulesRamSetup() reads the directory before setup() runs
    if (!region.valid && !findArea()) {
        return Area{0, 0, 0};
    }

    Area out{region.start, region.lo, region.hi};
    out.initial = region.initial;
    std::memcpy(out.keep, region.keep, sizeof(out.keep));
    return out;
}

bool rawRead(uint32_t offset, void* data, size_t size) {
    return lt_flash_read(offset, static_cast<uint8_t*>(data), size) == size;
}

bool rawErase(uint32_t offset, uint32_t size) {
    for (uint32_t done = 0; done < size; done += SectorSize) {
        if (!lt_flash_erase_block(offset + done)) {
            return false;
        }
    }

    return true;
}

bool rawWrite(uint32_t offset, const void* data, size_t size) {
    return lt_flash_write(offset, static_cast<const uint8_t*>(data), size) == size;
}

// where the module runs: the logical address of the first block in its slot, its blocks inside the slot
uint32_t imageAt(const Entry& entry) {
    const uint32_t address = entry.bases[BaseFlash];
    if (!region.valid || !key_found || (address != logicalFrom(entry.offset))
        || ((physical(address) + (roundUp(entry.size, BlockData) / BlockData * BlockSize)) > (entry.offset + entry.slot)))
    {
        return 0;
    }

    return address;
}

// the bytes as the module runs them, through the mapping; every block raw first (a block with a bad CRC stalls the
// chip when read through the mapping)
bool imageRead(uint32_t at, void* data, size_t size) {
    if (!key_found || !blocksValid(at & ~(BlockData - 1), (at & (BlockData - 1)) + size)) {
        return false;
    }

    std::memcpy(data, reinterpret_cast<const void*>(at), size);
    return true;
}

// whole sectors, the first block from the first block boundary in them
uint32_t imageFlash(uint32_t image) {
    return roundUp(BlockSize + (roundUp(image, BlockData) / BlockData * BlockSize), SectorSize);
}

uint32_t flashView(uint32_t slot) {
    return logicalFrom(slot);
}

uint32_t codeView(uint32_t) {
    return 0;
}

const char* imageBegin(Upload& up) {
    if (!region.valid) {
        return PSTR("no module area (OTA partition)");
    }

    if (!key_found) {
        return PSTR("the flash key of this chip is unknown");
    }

    if (up.header.image_size % BlockData) {
        return PSTR("bad image size");
    }

    // an OTA image left in the partition: with its header erased the bootloader sees no update to install
    uint8_t probe[16];
    if (lt_flash_read(region.download, probe, sizeof(probe)) == sizeof(probe)) {
        bool blank = true;
        for (const auto byte : probe) {
            blank = blank && (byte == 0xff);
        }
        if (!blank && !lt_flash_erase_block(region.download)) {
            return PSTR("flash erase");
        }
    }

    writing = Writing{};
    writing.slot = up.slot;
    writing.erased = up.slot;
    writing.end = up.slot + imageFlash(up.header.image_size);
    return nullptr;
}

const char* imageWrite(Upload& up, uint32_t offset, uint8_t* data, size_t size, bool last) {
    const auto* error = writeChunk(up, offset, data, size);
    if (!error && last && writing.carry_pending) {
        error = PSTR("a branch past the image end");
    }

    return error;
}

// the mapping keeps what it read: emptied before the image is read back
void imageDone(uint32_t, uint32_t) {
    evictCache();
}

// 34 byte blocks across the sectors: no rewrite in place
bool rewritable() {
    return false;
}

// the free data TCM is the fixed pool, the rest in the heap block
void pools(Pools& out) {
    // the free heap (lt_heap_get_max_alloc() is a stub here, 0; heap_4 keeps its free list to itself). Never probed by
    // malloc(): every refused malloc() runs the failed hook, which leaves a crash record and restarts the board (the
    // "malloc failed ... async_tcp" records after each Apply, 11-11). The block is taken at the next boot, before the
    // heap is cut up, so the free heap of now is a fair measure of what it can get then
    const uint32_t free = lt_heap_get_free();
    out.heap = heapBlockSize() + ((free > MODULES_HEAP_KEEP) ? (free - MODULES_HEAP_KEEP) : 0);
    out.fixed = tcmSize();
    out.fixed_base = tcmStart();
    out.fixed_words = false;
    out.code_in_fixed = false;
    out.words = false;
    out.name = "TCM";
}

void statusJson(String& out) {
    jsonField(out, F("chip"), lt_flash_get_size());
}

void listHead(Print& out) {
    out.printf_P(PSTR("beken: key %s, TCM pool %u bytes at %08x, heap block %u bytes at %08x\n"),
        key_name, tcmSize(), tcmStart(), heapBlockSize(), heapBlockBase());
}

} // namespace arch
} // namespace modules
} // namespace espurna

#endif // MODULES_SUPPORT && LIBRETINY
