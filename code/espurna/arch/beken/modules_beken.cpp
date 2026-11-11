/*

PROTOTYPE: the BK7231N backend of the module loader (modules_core.h, modules.cpp; docs/modules.md, "Beken")

Flash: the modules lie in the bootloader's 'download' partition (the OTA image goes there only while the head updates
itself, the modules are erased for that, as on the ESP8266). Its first sector stays free (the RBL header the bootloader
looks at), its last sector is the directory (raw bytes) with the guard's boot marks in its second half. Modules are
placed from the top down, in whole sectors.

Code runs from the flash through the mapping of the flash controller (XIP): logical address L is stored at flash
offset L / 32 * 34 as 32 bytes encrypted with the chip's key, then their CRC-16 (big endian). The board writes a
module in that format itself: the words are encrypted with the key found at boot (compared against its own app),
the CRC of every 32 bytes follows them. A module starts on a 32 byte boundary of the logical space, inside its sectors
(Entry::arch[0]: that logical address).

A module is relocated on install for the address it will have: its flash (kind 1), its RAM (kind 2), and every
BL / BLX from it into the head is moved by the flash delta (the branch list of the package, read by the core's package
reader). Module RAM, placed by the Apply largest first: the data TCM the head leaves free (from _tcmbss_end, the TCM is
not heap), the rest in the core's heap block. Nothing kept in reserve. The block is taken early at every boot at the
same address; when it moved anyway, the modules in it stay off (Apply again: the image would need rewriting).

*/

#include "espurna.h"
#include "modules.h"

#if MODULES_SUPPORT && MODULES_CORE && defined(LIBRETINY)

#include "modules_core.h"
#include "terminal.h"

#include "layout_beken.h"

#include <FreeRTOS.h>
#include <timers.h>

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

// GNU build id of the head link (scripts/beken_modules_head.py): note header (16 bytes), then the id
extern const uint8_t __build_id_start[];
}

namespace espurna {
namespace modules {
namespace {

constexpr uint32_t BlockData { 32 };
constexpr uint32_t BlockSize { 34 };
constexpr uint32_t DirectoryMagic { 0x33424445 }; // "EDB3": Beken, the Entry of modules_core.h, 16 entries
constexpr uint32_t PackageVersion { 3 };          // Beken: kinds 1 flash, 2 RAM; branch list per chunk
constexpr size_t Logical { 0 };                   // Entry::arch[Logical]: where the module runs
constexpr size_t RamAddress { 1 };                // Entry::arch[RamAddress]: the address its RAM words were relocated for

// branch list entries: offset in the chunk | kind << 16
constexpr uint32_t BranchThumb { 1 };   // Thumb BL / BLX pair (two half words)
constexpr uint32_t BranchArm { 2 };     // ARM BL / BLX
constexpr size_t BranchesMax { 1024 };  // per 4 KiB chunk

// the fixed pool: the free data TCM
uint32_t tcmStart() {
    return roundUp(reinterpret_cast<uint32_t>(_tcmbss_end), 8);
}

uint32_t tcmSize() {
    const uint32_t end = reinterpret_cast<uint32_t>(_espurna_tcm_end) & ~7u;
    return (end > tcmStart()) ? (end - tcmStart()) : 0;
}

// where the RAM of the module is this boot: the TCM, or the core's heap block
uint32_t ramAddress(uint32_t ram_offset) {
    const uint32_t offset = ram_offset & ~RamSecondArena;
    return ((ram_offset & RamSecondArena) ? tcmStart() : heapBlockBase()) + offset;
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
    uint32_t start { 0 };   // the download partition (for the meters)
    uint32_t lo { 0 };      // first sector for modules
    uint32_t hi { 0 };      // the directory sector
    bool valid { false };
};

Region region;

bool findArea() {
    const auto& layout = ::espurna::beken::flashLayout();
    if ((layout.layout == ::espurna::beken::Layout::Unknown) || (layout.download_length < (SectorSize * 4))) {
        return false;
    }

    region.start = layout.download_offset;
    region.lo = layout.download_offset + SectorSize;
    region.hi = layout.download_offset + layout.download_length - SectorSize;
    region.valid = region.hi > region.lo;
    return region.valid;
}

// flash of a module: whole sectors, its first block from the first block boundary in them
uint32_t slotSize(uint32_t image) {
    return roundUp(BlockSize + (roundUp(image, BlockData) / BlockData * BlockSize), SectorSize);
}

void commandsSetup();

// --- the head --------------------------------------------------------------------------------------

const uint8_t* buildId() {
    return __build_id_start + 16;
}

// --- the guard's boot marks ---------------------------------------------------------------------------

// The bootloader clears the whole RAM at every reset, so the guard's count lives in the directory sector: a pair of
// words per boot that started module code, written without an erase (bits only go 1 -> 0): [0] cleared when the code
// starts, [1] once the boot ran MODULES_BOOT_SECONDS or ended by a restart of ESPurna. A directory write erases them
constexpr uint32_t MarksOffset { 2048 };
constexpr uint32_t MarksSize { 512 };
constexpr size_t Marks { MarksSize / 8 };

bool readMarks(uint32_t marks[Marks * 2]) {
    return lt_flash_read(region.hi + MarksOffset, reinterpret_cast<uint8_t*>(marks), Marks * 8) == (Marks * 8);
}

bool clearMark(size_t word) {
    const uint32_t zero { 0 };
    return lt_flash_write(region.hi + MarksOffset + (word * 4), reinterpret_cast<const uint8_t*>(&zero), 4) == 4;
}

// boots in a row that started module code and did not get to their end mark
size_t unfinishedBoots(const uint32_t marks[Marks * 2]) {
    size_t out = 0;
    for (size_t index = 0; index < Marks; ++index) {
        if (marks[index * 2] != 0) {
            break;
        }
        out = (marks[index * 2 + 1] != 0) ? (out + 1) : 0;
    }

    return out;
}

// a restart of ESPurna, not a crash: a custom reason, except Stability (the exit of the unstable mode, a boot without
// modules: the last mark is of the crashed boot before it)
bool deliberateRestart() {
    const auto reason = customResetReason();
    return (reason != CustomResetReason::None) && (reason != CustomResetReason::Stability);
}

arch::GuardState guard {};
size_t boot_mark { Marks };
bool boot_mark_finished { false };

// the core's view (running / booted / crashes of the boot before this one) made from the marks
void guardLoad() {
    uint32_t marks[Marks * 2];
    if (!region.valid || !readMarks(marks)) {
        return;
    }

    size_t last = Marks;
    for (size_t index = 0; index < Marks; ++index) {
        if (marks[index * 2] == 0) {
            last = index;
        }
    }

    // ended by a restart of ESPurna: that boot is finished
    if ((last < Marks) && (marks[last * 2 + 1] != 0) && deliberateRestart()) {
        clearMark(last * 2 + 1);
        marks[last * 2 + 1] = 0;
    }

    const auto unfinished = unfinishedBoots(marks);
    guard.running = (last < Marks) ? 1 : 0;
    guard.booted = ((last < Marks) && (marks[last * 2 + 1] == 0)) ? 1 : 0;
    // the core adds the last boot when it crashed
    guard.crashes = (guard.running && !guard.booted && unfinished) ? (unfinished - 1) : unfinished;
}

// module code starts in this boot
void guardMark() {
    uint32_t marks[Marks * 2];
    if (!readMarks(marks)) {
        return;
    }

    size_t index = 0;
    while ((index < Marks) && (marks[index * 2] == 0)) {
        ++index;
    }

    // full: the sector again without the marks (the directory kept)
    if (index == Marks) {
        Directory dir;
        if (!readDirectory(dir) || !writeDirectoryPrimary(dir)) {
            return;
        }
        index = 0;
    }

    if (clearMark(index * 2)) {
        boot_mark = index;
    }
}

TimerHandle_t guard_timer { nullptr };

void guardTimerCallback(TimerHandle_t) {
    guardTick();

    if (guard.booted && !boot_mark_finished && (boot_mark < Marks)) {
        boot_mark_finished = true;
        clearMark(boot_mark * 2 + 1);
    }
}

// --- upload: placed when its header is in, a whole chunk relocated (words, branches), encrypted and written -------

struct Upload : PackageReader {
    uint8_t chunk[ChunkSize];
    uint32_t branch_list[BranchesMax];
    uint8_t carry[BlockData];       // the last block of the previous chunk, while a branch spans into this one

    uint32_t image_crc { 0 };
    uint32_t slot { 0 };            // flash offset
    uint32_t slot_size { 0 };
    uint32_t address { 0 };         // logical address
    uint32_t ram_offset { 0 };
    uint32_t delta[3] {};           // by kind: 0, flash, RAM

    uint32_t chunk_size { 0 };
    uint32_t done { 0 };            // image bytes handled (whole chunks)
    uint32_t erased { 0 };          // flash erased up to here

    bool carry_pending { false };
    uint32_t carry_address { 0 };
};

const char* prepare(PackageReader& reader) {
    auto& up = static_cast<Upload&>(reader);
    const auto& header = up.header;
    if (!region.valid) {
        return PSTR("no module area (OTA partition)");
    }

    if (!key_found) {
        return PSTR("the flash key of this chip is unknown");
    }

    Directory dir;
    bool installed = false;
    const auto* error = packageAccept(up, PackageVersion, dir, installed);
    if (error) {
        return error;
    }

    if ((header.image_size % BlockData) || header.iram_size) {
        return PSTR("bad image size");
    }

    Spaces space;
    spaces(dir, true, space, installed ? header.name : nullptr);

    up.slot_size = slotSize(header.image_size);
    if (!space.flash.place(up.slot_size, true, up.slot)) {
        return PSTR("not enough flash");
    }

    // where the Apply placed it; else (an old single upload) into what the TCM has left: the heap block of this boot was
    // sized before, it has no room for more
    const uint32_t ram_size = roundUp(header.ram_size, 8);
    if (ram_size && up.planned) {
        up.ram_offset = up.planned_ram;
    } else if (ram_size) {
        uint32_t offset;
        if (!space.ram_second.place(ram_size, false, offset)) {
            return PSTR("no room in the TCM for its RAM: Apply in the MODULES panel");
        }
        up.ram_offset = offset | RamSecondArena;
    } else {
        up.ram_offset = 0;
    }

    // an OTA image left in the partition: with its header erased the bootloader sees no update to install
    uint8_t probe[16];
    if (lt_flash_read(region.start, probe, sizeof(probe)) == sizeof(probe)) {
        bool blank = true;
        for (const auto byte : probe) {
            blank = blank && (byte == 0xff);
        }
        if (!blank && !lt_flash_erase_block(region.start)) {
            return PSTR("flash erase");
        }
    }

    up.address = logicalFrom(up.slot);
    up.erased = up.slot;
    up.delta[0] = 0;
    up.delta[1] = up.address - header.link_flash;
    up.delta[2] = ramAddress(up.ram_offset) - header.link_ram;

    return nullptr;
}

bool eraseTo(Upload& up, uint32_t end) {
    while (up.erased < end) {
        if (!flashErase(up.erased, SectorSize)) {
            return false;
        }
        up.erased += SectorSize;
    }

    return true;
}

bool writePlain(Upload& up, uint32_t address, const uint8_t* data, size_t size) {
    const uint32_t end = physical(address) + (size / BlockData * BlockSize);
    if ((end > (up.slot + up.slot_size)) || !eraseTo(up, end)) {
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

// a whole chunk is in: relocate, move the branches, write it
const char* chunkDone(Upload& up) {
    const uint32_t size = up.chunk_size;
    for (uint32_t index = 0; index < (size / 4); ++index) {
        const uint8_t kind = (up.map[index >> 2] >> ((index & 3) * 2)) & 3;
        if (kind == 3) {
            return PSTR("unknown relocation kind");
        }
        if (kind) {
            uint32_t value;
            std::memcpy(&value, &up.chunk[index * 4], 4);
            value += up.delta[kind];
            std::memcpy(&up.chunk[index * 4], &value, 4);
        }
    }

    const uint32_t address = up.address + up.done;
    const uint32_t delta = up.delta[1];

    // a Thumb branch from the end of the previous chunk into this one
    if (up.carry_pending) {
        const auto* error = moveThumb(&up.carry[BlockData - 2], &up.chunk[0], delta);
        if (error) {
            return error;
        }
        if (!writePlain(up, up.carry_address, up.carry, BlockData)) {
            return PSTR("flash write");
        }
        up.image_crc = crc32Update(up.image_crc, up.carry, BlockData);
        up.carry_pending = false;
    }

    bool spans = false;
    for (uint32_t index = 0; index < up.branch_count; ++index) {
        const uint32_t offset = up.branch_list[index] & 0xffff;
        const uint32_t kind = up.branch_list[index] >> 16;
        const char* error { nullptr };
        if ((kind == BranchThumb) && ((offset + 2) == size) && (size == ChunkSize)) {
            spans = true;
            continue;
        }
        if ((kind == BranchThumb) && ((offset + 4) <= size) && !(offset & 1)) {
            error = moveThumb(&up.chunk[offset], &up.chunk[offset + 2], delta);
        } else if ((kind == BranchArm) && ((offset + 4) <= size) && !(offset & 3)) {
            error = moveArm(&up.chunk[offset], delta);
        } else {
            error = PSTR("branch list: bad entry");
        }
        if (error) {
            return error;
        }
    }

    const uint32_t now = spans ? (size - BlockData) : size;
    if (!writePlain(up, address, up.chunk, now)) {
        return PSTR("flash write");
    }
    up.image_crc = crc32Update(up.image_crc, up.chunk, now);

    if (spans) {
        std::memcpy(up.carry, &up.chunk[size - BlockData], BlockData);
        up.carry_address = address + size - BlockData;
        up.carry_pending = true;
    }

    up.done += size;
    return nullptr;
}

// image bytes of the current chunk: collected, the whole chunk handled at its end
const char* bytes(PackageReader& reader, const uint8_t* data, size_t size) {
    auto& up = static_cast<Upload&>(reader);
    const uint32_t offset = up.delivered % ChunkSize;
    std::memcpy(&up.chunk[offset], data, size);
    if (size < up.chunk_left) {
        return nullptr;
    }

    up.chunk_size = offset + size;
    return chunkDone(up);
}

constexpr PackageHooks Hooks { prepare, bytes };

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

// --- the contract of modules_core.h ------------------------------------------------------------------------------

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

uint32_t directoryMagic() {
    return DirectoryMagic;
}

// Flash = the OTA partition, its first sector shown as the "head"
Area area() {
    // modulesRamSetup() reads the directory before setup() runs
    if (!region.valid && !findArea()) {
        return Area{0, 0, 0};
    }

    return Area{region.start, region.lo, region.hi};
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
    const uint32_t address = entry.arch[Logical];
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

// the first 16 bytes of the head's GNU build id (the module header's head_md5 field)
bool headId(uint8_t out[16]) {
    std::memcpy(out, buildId(), 16);
    return true;
}

// from the mapping (code, header, the .data image), RAM in the TCM or the heap block; no IRAM
const char* runs(const Entry& entry, const Header&, Runs& out) {
    if (entry.ram_size && (ramAddress(entry.ram_offset) != entry.arch[RamAddress])) {
        return PSTR("the heap block moved since the Apply: Apply again");
    }
    if (entry.ram_size && !(entry.ram_offset & RamSecondArena)
        && ((entry.ram_offset + entry.ram_size) > heapBlockSize()))
    {
        return PSTR("not in this boot's heap block: Apply again");
    }

    const uint32_t address = entry.arch[Logical];
    out.flash_lo = address;
    out.flash_hi = address + entry.size;
    out.entry_lo = out.flash_lo;
    out.entry_hi = out.flash_hi;
    out.ram_lo = ramAddress(entry.ram_offset);
    out.ram_hi = out.ram_lo + entry.ram_size;
    out.iram_lo = 0;
    out.iram_hi = 0;
    out.iram_copy = false;
    out.words = false;
    return nullptr;
}

// EntryUnplaced (the full image, or switched off): the image in flash is encrypted, its RAM words point to its place in
// the arena (the full image gives every module its own place, scripts/beken_full_image.py; an upload the one best fit
// found then). Rewriting the slot for another place would be a flash rewrite of the whole image with no copy to go
// back to on a power cut, so the place stays: switched on, it runs when no module placed now takes any of that RAM
const char* place(const Directory& dir, Entry& entry) {
    if (!imageAt(entry)) {
        return PSTR("not in the module area");
    }

    if (!(entry.ram_offset & RamSecondArena) || (((entry.ram_offset & ~RamSecondArena) + entry.ram_size) > tcmSize())) {
        return PSTR("RAM outside the TCM: Apply in the MODULES panel");
    }

    const uint32_t lo = entry.ram_offset;
    const uint32_t hi = lo + entry.ram_size;
    for (uint32_t index = 0; index < dir.count; ++index) {
        const auto& other = dir.entries[index];
        if ((&other == &entry) || (0 == std::strncmp(other.name, entry.name, sizeof(entry.name)))
            || (other.flags & EntryUnplaced) || !other.ram_size || !hi)
        {
            continue;
        }
        if ((other.ram_offset < hi) && (lo < (other.ram_offset + other.ram_size))) {
            return PSTR("not enough RAM: another module switched on takes its RAM");
        }
    }

    if (!blocksValid(entry.arch[Logical], entry.size)) {
        return PSTR("flash blocks of the image");
    }

    return nullptr;
}

// its RAM stays where it was placed (place() above): it can always be given back and taken again
bool placeable(const Entry&) {
    return region.valid && key_found;
}

PackageReader* uploadNew(AsyncWebServerRequest*) {
    auto* out = new (std::nothrow) Upload();
    if (out) {
        out->hooks = &Hooks;
        out->branches = out->branch_list;
        out->branches_max = BranchesMax;
    }

    return out;
}

const char* uploadFinish(PackageReader& reader, Entry& out) {
    auto& up = static_cast<Upload&>(reader);
    const auto* error = packageComplete(up);
    if (error) {
        return error;
    }

    if ((up.done != up.header.image_size) || up.carry_pending) {
        return PSTR("short upload");
    }

    // what the code will read: every block raw first, then through the mapping (its cache emptied first)
    evictCache();
    uint32_t crc;
    if (!imageCrc(up.address, up.done, crc)) {
        return PSTR("verify: flash blocks");
    }

    if (crc != up.image_crc) {
        return PSTR("verify: mapped image");
    }

    out.offset = up.slot;
    out.slot = up.slot_size;
    out.size = up.done;
    out.crc32 = crc;
    out.ram_offset = up.ram_offset;
    out.ram_size = roundUp(up.header.ram_size, 8);
    out.arch[Logical] = up.address;
    out.arch[RamAddress] = ramAddress(up.ram_offset);
    return nullptr;
}

uint32_t slotFor(uint32_t image) {
    return slotSize(image);
}

// the free data TCM is the fixed pool, the rest of the RAM in the heap block
void pools(Pools& out) {
    // the free heap (lt_heap_get_max_alloc() is a stub here, 0; heap_4 keeps its free list to itself). Never probed by
    // malloc(): every refused malloc() runs the failed hook, which leaves a crash record and restarts the board (the
    // "malloc failed ... async_tcp" records after each Apply, 11-11). The block is taken at the next boot, before the
    // heap is cut up, so the free heap of now is a fair measure of what it can get then
    const uint32_t free = lt_heap_get_free();
    out.heap = heapBlockSize() + ((free > MODULES_HEAP_KEEP) ? (free - MODULES_HEAP_KEEP) : 0);
    out.fixed = tcmSize();
    out.fixed_words = false;
    out.code_in_fixed = false;
}

void arenas(const Directory&, bool, Spaces& out, const char*) {
    out.ram_second.lo = 0;
    out.ram_second.hi = tcmSize();
}

void statusJson(String& out) {
    out += F(",\"pool\":\"TCM\"");
    jsonField(out, F("chip"), lt_flash_get_size());
}

void entryJson(String& out, const Entry& entry) {
    jsonField(out, F("address"), entry.arch[Logical]);
}

void listHead(Print& out) {
    out.printf_P(PSTR("loader beken rev 4 (core), key %s, TCM pool %u bytes at %08x, heap block %u bytes at %08x\n"),
        key_name, tcmSize(), tcmStart(), heapBlockSize(), heapBlockBase());

    uint32_t marks[Marks * 2];
    if (region.valid && readMarks(marks)) {
        size_t used = 0;
        while ((used < Marks) && (marks[used * 2] == 0)) {
            ++used;
        }
        out.printf_P(PSTR("boot marks: %u of %u, unfinished in a row: %u\n"), used, Marks, unfinishedBoots(marks));
    }
}

void listEntry(Print& out, const Entry& entry) {
    out.printf_P(PSTR(", runs at 0x%06x, slot %u"), entry.arch[Logical], entry.slot);
}

// the boot before this one did not end by a restart of ESPurna (a crash, the watchdog, power)
bool crashRestart() {
    return customResetReason() == CustomResetReason::None;
}

GuardState& guardState() {
    static bool loaded { false };
    if (!loaded) {
        loaded = true;
        guardLoad();
    }

    return guard;
}

// a FreeRTOS timer (its own task: it ticks while loop() waits); the start mark of this boot first
void guardTimer() {
    guardMark();
    if (!guard_timer) {
        guard_timer = xTimerCreate("modules", pdMS_TO_TICKS(1000), pdTRUE, nullptr, guardTimerCallback);
    }
    if (guard_timer) {
        xTimerStart(guard_timer, 0);
    }
}

void guardTimerStop() {
    if (guard_timer) {
        xTimerStop(guard_timer, 0);
    }
}

void restartNow() {
    customResetReason(CustomResetReason::Stability);
    espurna::system::arch::restart();
}

uint32_t freeHeap() {
    return systemFreeHeap();
}

} // namespace arch
} // namespace modules
} // namespace espurna

#endif // MODULES_SUPPORT && MODULES_CORE && LIBRETINY
