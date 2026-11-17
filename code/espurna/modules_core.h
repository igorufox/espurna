/*

PROTOTYPE: the shared core of the module loader (docs/modules.md) and what a chip provides to it

One loader for every chip. modules.cpp is the core: the directory and its entries, the checks of a module image (its
header, its head, its CRC, its ranges) and its start (data, bss, constructors, entry), what each module did at this
boot, the guard (its boot marks in the directory sector, low heap, failing allocations, a stalled loop), the package
reader, the relocation (as a package arrives, and again at boot when the bases moved), the Apply, the /modules web
endpoints and their JSON, the MODULES terminal commands, the start order and the data modules (the WebUI).

A chip (arch/esp8266/modules_esp8266.cpp, arch/esp32/modules_esp32.cpp, arch/beken/modules_beken.cpp) has only what the hardware
forces: the functions of namespace `arch` below.

*/

#pragma once

#include "modules.h"

#include <Arduino.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

class Print;

namespace espurna {
namespace modules {

// 16 modules at most. A change of it is a new directory format: new magic
// 24 (owner 11-16: 16 is too few, an NSPanel already runs 16): the directory 1744 bytes, still in the first half of
// its sector (the marks stay where they are) and small enough for the stack of setup()
constexpr size_t DirectoryEntries { 24 };
constexpr uint32_t SectorSize { 4096 };

// a big temporary (Directory ~1.1 KB, Spaces ~0.8 KB) on the heap: the ESP8266 web callbacks run on the 4 KB system
// stack, onStatus held a Directory, Spaces and offReason()'s Directory at once (Exception 29 in onStatus, 11-11)
template <typename T>
std::unique_ptr<T> boxed() {
    return std::unique_ptr<T>(new (std::nothrow) T());
}

// one installed module, the same on every chip
struct Entry {
    char name[16];
    uint32_t offset;        // flash offset of its slot
    uint32_t slot;          // flash bytes its slot takes, whole sectors: the image (in the chip's flash format), then
                            // its relocation maps (the boot relocates it from them when its bases moved)
    uint32_t size;          // image bytes
    uint32_t crc32;         // of the image as it runs
    uint32_t ram_offset;    // its statics (.data + .bss) in the heap block; RamInFixedPool set: in the fixed pool
    uint32_t ram_size;
    uint32_t code_size;     // its IRAM code in the fixed pool (ESP8266), else 0
    uint32_t bases[3];      // the addresses its words hold now: BaseFlash, BaseCode, BaseRam
    uint32_t flags;         // EntryDisabled
    uint32_t data_offset;   // the flash it keeps its data in (the data column of ESPURNA_MODULES, modulesData()),
    uint32_t data_size;     // whole sectors at the top of the area, placed by the Apply; 0 - none
};

// Entry::bases: the image as data (kind 1 words), its code (kind 3: ESP8266 its IRAM code in the fixed pool, ESP32 the
// instruction view of the slot, Beken 0), its statics (kind 2)
constexpr size_t BaseFlash { 0 };
constexpr size_t BaseCode { 1 };
constexpr size_t BaseRam { 2 };

// switched off: installed, its statics placed by the Apply, not started. On and off take effect at the next boot
constexpr uint32_t EntryDisabled { 1 };

// Entry::ram_offset: the statics of the module are in the chip's fixed pool (arch::Pools), the rest of the word is
// the offset in it
constexpr uint32_t RamInFixedPool { 0x80000000 };

// the directory format, one for every chip
constexpr uint32_t DirectoryMagic { 0x38524445 }; // "EDR8": 24 entries (EDR7: 16, Entry::data_offset, data_size)

struct Directory {
    uint32_t magic;         // DirectoryMagic
    uint32_t count;
    Entry entries[DirectoryEntries];
    // the guard: not empty - the modules stay off, why. Here and not in the settings: EEPROM_Rotate went back to an
    // older copy of the settings after the crashes and the unstable mode, and the modules started again
    char off[96];
    // two copies, in the last sector of the area and the one under it: a write goes into the older one, a read takes
    // the newest whole one. A power cut during a write loses only that write
    uint32_t generation;
    uint32_t crc;           // CRC-32 of everything before it
};

// the sector under the directory holds its second copy: no module slot there
constexpr uint32_t DirectorySectors { 2 };

// the directory sector: the directory, the guard's boot marks at 2048, the power marks at 3072
static_assert(sizeof(Directory) <= 2048, "the directory sector keeps the boot marks in its second half");

enum class State {
    None,
    Skipped,    // unstable mode
    Off,        // the guard
    Started,
    Stale,      // made for another head image
    Broken,     // bad header / crc / ranges
    Disabled,   // switched off (EntryDisabled)
};

struct Status {
    State state { State::None };
    const char* reason { nullptr };
    uint32_t offset { 0 };  // of the entry at boot: the directory changes before the restart, the indexes move
    uint32_t crc32 { 0 };
    uint32_t heap { 0 };    // free heap its start took
    char name[16] {};
};

// what this boot did with the module at this flash offset
Status statusOf(const Entry& entry);
const __FlashStringHelper* stateName(const Status& status);

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t size);
uint32_t roundUp(uint32_t value, uint32_t to);
bool insideRange(uint32_t start, uint32_t end, uint32_t lo, uint32_t hi);
void jsonField(String& out, const __FlashStringHelper* name, uint32_t value);

bool readDirectory(Directory& out);

// the heap block of the modules' statics this boot (modulesRamSetup): its address (also with nothing in it: where it
// would be, the Apply places for it), its size; what the directory's modules need of it
uint32_t heapBlockBase();
uint32_t heapBlockSize();
uint32_t heapNeed(const Directory& dir);
bool writeDirectory(const Directory& dir);
bool writeDirectoryPrimary(const Directory& dir);
String offReason();

// writes into the module area and its directory sector only (arch::area()), erases in whole sectors
bool flashErase(uint32_t offset, uint32_t size);
bool flashWrite(uint32_t offset, const void* data, size_t size);

// the CRC of `size` image bytes at `at` (arch::imageAt), read as the module runs them
bool imageCrc(uint32_t at, uint32_t size, uint32_t& out);

// the header of an installed module; its header, its head, its CRC checked (nullptr), else why not: PSTR("stale") -
// made for another head
bool readHeader(const Entry& entry, Header& out);
const char* checkImage(const Entry& entry, Header& out);

// the head that runs (its build id, modules.cpp): is it this one
bool sameHead(const uint8_t id[16]);

// the space of one kind (flash between the head and the directory, a pool) with the modules' pieces in it
struct Space {
    uint32_t lo { 0 };
    uint32_t hi { 0 };
    uint32_t start[DirectoryEntries * 2];
    uint32_t end[DirectoryEntries * 2];
    size_t count { 0 };

    void add(uint32_t from, uint32_t size);
    uint32_t gapStart(size_t index) const;
    uint32_t gapEnd(size_t index) const;
    uint32_t gap(size_t index) const;
    uint32_t free() const;
    uint32_t largest() const;

    // best fit: the smallest gap that holds it; flash at the top of the gap (the head grows from below)
    bool place(uint32_t size, bool top, uint32_t& out) const;
};

// what the modules take: the flash, the heap block, the chip's fixed pool (statics and IRAM code)
struct Spaces {
    Space flash;
    Space ram;          // the heap block, as big as the heap may give it
    Space fixed;
};

void spaces(const Directory& dir, bool valid, Spaces& out);

// --- a package ------------------------------------------------------------------------------------------------------

// the same format on every chip: its header, then per 4 KiB chunk of the image the chunk's map (2 bits per word: 0 as
// is, 1 flash, 2 RAM, 3 code), the chunk's branch list (a count, then offset | kind << 16 per branch into the head:
// Beken's Thumb / ARM BL / BLX; none on the other chips; and the links to the modules it needs, every chip: LinkWord,
// LinkBranch), then the chunk's bytes
constexpr uint32_t PackageVersion { 5 };
constexpr size_t BranchesMax { 1024 };  // per chunk

// A module is linked against the modules it needs (Header::needs) as against the head (scripts/module_link.py): its
// words with an address of one of them get where the Apply put that module. Kinds of the branch list:
//   LinkWord | provider << 2 | base kind (1 flash, 2 RAM, 3 code): the word + where that base of the provider is
//   LinkBranch | provider << 2 | branch kind (Beken: 1 Thumb, 2 ARM BL / BLX): a branch into the provider
// provider: the index in the needs of the module. The slot keeps them behind its maps (a count, then image offset |
// kind << 24 each): a boot rewrites the module when a provider moved
constexpr uint32_t LinkWord { 0x10 };
constexpr uint32_t LinkBranch { 0x20 };
constexpr size_t LinkProviders { 4 };

constexpr bool isLink(uint32_t kind) {
    return (kind >= LinkWord) && (kind < (LinkBranch + 0x10));
}

// one package being written: its statics and IRAM code where the Apply's plan put them (arch::pools, largest first),
// its slot from the top of the area down
struct Upload {
    PackageHeader header {};
    uint8_t map[MapSize] {};
    std::unique_ptr<uint32_t[]> branches;   // the chunk's branch list, taken when a chunk has one
    uint32_t branches_size { 0 };           // entries `branches` holds
    uint32_t branch_count { 0 };
    alignas(4) uint8_t chunk[ChunkSize];
    uint32_t chunk_fill { 0 };

    size_t received { 0 };      // bytes of the package
    uint32_t body_crc { 0 };    // after the header: maps, branch lists and chunks (PackageHeader::crc32)
    uint32_t delivered { 0 };   // image bytes taken
    size_t fill { 0 };          // of the map / count / branch list
    size_t chunk_left { 0 };    // image bytes of the current chunk still to come
    uint8_t part { 0 };

    const Directory* apply_dir { nullptr };    // the Apply's directory in memory: the slot goes by it
    uint32_t flash_floor { 0 }; // no slot under it (the initial image's modules file)
    uint32_t flash_ceiling { 0 };   // no slot over it (the data of the modules, placed first), 0 - the area's top
    uint32_t data_offset { 0 }; // its data, planned (modulesData())
    uint32_t data_size { 0 };
    uint32_t ram_offset { 0 };  // planned (RamInFixedPool: in the fixed pool)
    uint32_t code_offset { 0 }; // planned, in the fixed pool (ESP8266)

    uint32_t slot { 0 };        // flash offset of the slot
    uint32_t slot_size { 0 };
    uint32_t bases[3] {};       // the Entry::bases its words get
    uint32_t delta[4] {};       // by kind
    char needs[32] {};          // the modules it needs (the modules file), the providers of its links
    uint32_t provider[LinkProviders][4] {};    // the deltas of each provider, by kind: where the Apply put it
    bool provider_known[LinkProviders] {};
    uint32_t links_written { 0 };
    uint32_t written { 0 };     // image bytes handed to arch::imageWrite
    uint32_t image_crc { 0 };   // of the image as it will run (arch::imageWrite adds to it)

    const char* error { nullptr };
};

// a whole chunk relocated by the deltas of its kinds, in place
void relocateChunk(uint8_t* data, size_t size, const uint8_t* map, const uint32_t delta[4]);

// the word of a link (LinkWord) at `at` in the chunk moved by its provider's delta; false - not a word in the chunk
bool linkWord(uint8_t* data, size_t size, uint32_t at, uint32_t delta);

// --- the guard -----------------------------------------------------------------------------------------------------

// the guard's timer calls it every second
void guardTick();

// --- what a chip provides ------------------------------------------------------------------------------------------
//
// Only what the hardware forces: where the area is, raw flash access and the chip's flash format, where an image runs
// (the maps of the flash, XIP), the fixed pool. Everything else is the core's. "Default": a weak default in
// modules.cpp

namespace arch {

// first thing at boot (find the flash area, map it, the flash key)
void setup();

// MODULES.TEST.CRASH: an exception now. Default: a store to 0 (does not fault on every chip)
void crashNow();

// the module area in flash. start: where the Flash meter starts (ESP8266: 0, the head; ESP32: app0; Beken: the OTA
// partition), lo: the first byte for modules, hi: the directory sector, the last of the area. hi 0 - no area
struct Area {
    uint32_t start;
    uint32_t lo;
    uint32_t hi;
    // where the initial image put its modules file (module_initial_image.py), 0 - at lo. Beken: the area starts right
    // behind the head in the app partition, the flasher writes the file into the download partition
    uint32_t initial { 0 };
    // flash inside lo..hi no module may take (Beken: the RBL headers, the settings pool and calibration, the
    // bootloader's own sectors), size 0 - unused
    struct Range {
        uint32_t offset;
        uint32_t size;
    };
    static constexpr size_t KeepMax { 6 };
    Range keep[KeepMax] {};

    // bytes of lo..hi in the ranges kept
    uint32_t kept(uint32_t from, uint32_t to) const {
        uint32_t out { 0 };
        for (const auto& range : keep) {
            const uint32_t lo = (range.offset > from) ? range.offset : from;
            const uint32_t hi = ((range.offset + range.size) < to) ? (range.offset + range.size) : to;
            out += (range.size && (hi > lo)) ? (hi - lo) : 0;
        }
        return out;
    }
};

Area area();

// how far the head can erase and write: the whole area has to be below it before an Apply touches anything (Beken: the
// FAL 'root' partition of the build; a 4 MiB board under a 2 MiB head refused the erase half way, 2026-11-16)
uint32_t flashReach();

// the flash as it is stored (Beken: encrypted, a CRC-16 per 32 bytes); erase in whole sectors
bool rawRead(uint32_t offset, void* data, size_t size);
bool rawErase(uint32_t offset, uint32_t size);
bool rawWrite(uint32_t offset, const void* data, size_t size);

// where the image of a module is read as the module runs it, 0 - the entry does not match its slot; and those bytes.
// Default: plain flash (ESP8266, ESP32), its flash offset and rawRead
uint32_t imageAt(const Entry& entry);
bool imageRead(uint32_t at, void* data, size_t size);

// the flash an image of this size takes in its slot, whole sectors (Beken: 34 bytes per 32). Default: in sectors
uint32_t imageFlash(uint32_t image);

// the address the image in a slot is read at as data (kind 1), and run from as code when the chip has a second view
// of the flash (ESP32: the instruction view, kind 3), 0 - none (the code is in the data view or in the fixed pool)
uint32_t flashView(uint32_t slot);
uint32_t codeView(uint32_t slot);

// the address a module's data (modulesData()) at this flash offset is read at as plain memory, any byte at any
// address, 0 - none (Beken: the view decrypts, the data is stored plain; ESP8266: 32-bit reads only). Default: none
uint32_t dataView(uint32_t offset);

// a whole chunk of the image, relocated, `offset` from the image start, into the slot: Beken moves the branches of
// up.branches and writes its format, the others erase and write. Adds the bytes as they will run to up.image_crc.
// Default: plain flash
const char* imageWrite(Upload& up, uint32_t offset, uint8_t* data, size_t size, bool last);

// before the first chunk (Beken: the flash key, no OTA image left in the partition); after the image is written or
// rewritten (drop what the flash cache holds of it). Default: nothing
const char* imageBegin(Upload& up);
void imageDone(uint32_t slot, uint32_t size);


// the statics (.data + .bss) and IRAM code of the modules, placed at each Apply, no reserve kept: first the chip's pool
// at a fixed address (Beken: the free data TCM; ESP8266: the IRAM after the head's code, module code first, 32-bit
// access only), largest module first; the rest in one heap block that the next boot takes before the modules start
struct Pools {
    uint32_t heap;          // bytes the heap block may have: the free heap minus what the head keeps for itself
    uint32_t fixed;         // bytes of the fixed pool (RamInFixedPool), 0 - none
    uint32_t fixed_base;    // its address
    bool fixed_words;       // the fixed pool takes 32-bit access only: no module with ModuleByteRam there
    bool code_in_fixed;     // module IRAM code goes to the fixed pool from its start, copied there at the start
    bool words;             // 32-bit access only to the module's statics in the fixed pool and to the mapped flash
    const char* name;       // of the fixed pool, for the panel ("IRAM", "TCM"), nullptr - none
};

void pools(Pools& out);

// /modules: the chip's own numbers after the common ones. Default: none
void statusJson(String& out);

// MODULES: the chip's line after the head line. Default: none
void listHead(Print& out);

uint32_t failedAllocations();   // default 0: the chip does not count them

// an OTA is running: no Apply then. Default: the web upload
bool otaBusy();

// the guard's timer: guardTick() every second, also while loop() waits or hangs (ESP8266: an SDK timer,
// arch/esp8266/modules_esp8266.cpp; ESP32, Beken: a FreeRTOS timer, its own task, arch/rtos/modules_rtos.cpp)
void guardTimerStart();
void guardTimerStop();

} // namespace arch

} // namespace modules
} // namespace espurna
