/*

PROTOTYPE: the shared core of the module loader (docs/modules.md) and the contract of its backends

One loader for every chip. modules.cpp is the core: the directory and its entries, the checks of a module image (its
header, its head, its CRC, its ranges) and its start (data, bss, constructors, entry), what each module did at this
boot, the guard (crash count, low heap, failing allocations, a stalled loop), the package reader (header, maps, the
branch lists, CRC), the word-by-word relocating writer, placing (flash, arenas, best fit), the /modules web endpoints
and their JSON, the MODULES terminal commands, the start order and the data modules (the WebUI).

A backend per chip (modules_esp8266.cpp, arch/esp32/modules_esp32.cpp, arch/beken/modules_beken.cpp) has only what
depends on the hardware: where the area and the directory are, raw flash access (Beken: the encrypted format), where a
module runs (maps, XIP) and its arenas, the relocation (ESP8266 and ESP32 by words as the package comes, ESP32 again at
boot; Beken words + Thumb / ARM branches), the guard's timer and its memory across restarts. The functions in namespace
`arch` below are what a backend provides; the ones marked "default" have a weak default in modules.cpp.

A chip uses the core once its backend implements the contract: MODULES_CORE is 1 for it (modules.h).

*/

#pragma once

#include "modules.h"

#include <Arduino.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

class AsyncWebServerRequest;
class Print;

namespace espurna {
namespace modules {

// 16: every module of a chip in its full image (ESP32 has 14). A change of it is a new directory format: new magic
constexpr size_t DirectoryEntries { 16 };
constexpr uint32_t SectorSize { 4096 };

// a big temporary (Directory ~1.2 KB, Spaces ~1 KB) on the heap: the ESP8266 web callbacks run on the 4 KB system
// stack, onStatus held a Directory, Spaces and offReason()'s Directory at once (Exception 29 in onStatus, 11-11)
template <typename T>
std::unique_ptr<T> boxed() {
    return std::unique_ptr<T>(new (std::nothrow) T());
}

// one installed module: the common part, then the backend's own words
struct Entry {
    char name[16];
    uint32_t offset;        // flash offset of its slot (where its header is, on chips with plain flash)
    uint32_t slot;          // flash bytes its slot takes, whole sectors: what placing reserves
    uint32_t size;          // image bytes
    uint32_t crc32;         // of the image as it is in flash and runs
    uint32_t ram_offset;    // in its RAM arena; RamSecondArena set: in the chip's second RAM arena
    uint32_t ram_size;
    uint32_t arch[3];       // ESP8266: IRAM offset, IRAM size; ESP32: bases; Beken: logical address
    uint32_t flags;         // EntryDisabled, EntryUnplaced
};

// switched off: placed in flash, not started (the full image places every module like that); switched on, it starts
// at once, nothing to download, no restart
constexpr uint32_t EntryDisabled { 1 };

// the same on every chip: a module that is off takes no RAM. Its image lies in flash, its RAM (and the backend's other
// arenas) is not placed: no arena counts it. Switched on, the backend places it and relocates the image in its slot
// (arch::place), then it starts. Switched off (and not running), the core gives its RAM back when the backend can place
// it again without a download (arch::placeable). The full image of every chip has all modules like that
constexpr uint32_t EntryUnplaced { 2 };

// Entry::ram_offset: the RAM of the module is in the chip's second RAM arena (ESP8266: the IRAM after the head's code,
// chosen at the install), the rest of the word is the offset in it
constexpr uint32_t RamSecondArena { 0x80000000 };

struct Directory {
    uint32_t magic;         // the backend's (a chip, a format)
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

static_assert(sizeof(Directory) <= 2048, "the Beken directory sector keeps its boot marks in the second half");

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
    uint32_t crc32 { 0 };   // ... and a new copy may take the place of one that did not start
    uint32_t heap { 0 };    // free heap its start took
    char name[16] {};
};

// what this boot did with the module at this flash offset; one installed after the boot is not started
Status statusOf(const Entry& entry);
const __FlashStringHelper* stateName(const Status& status);

// a module (code or data) started at this boot from this flash offset; how many did
bool startedAt(uint32_t offset);
size_t startedCount();

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

// the 16 bytes of an MD5 in hex (ESP.getSketchMD5()); the head that runs; is it this one
bool hexId(const String& hex, uint8_t out[16]);
bool sameHead(const uint8_t id[16]);

// the space of one kind (flash between the head and the directory, an arena) with the modules' pieces in it
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

    // best fit: the smallest gap that holds it (a removed module's hole goes to one of its size, the big gap stays
    // for a big one); flash at the top of the gap (the head grows from below), the arenas at the bottom
    bool place(uint32_t size, bool top, uint32_t& out) const;
};

struct Spaces {
    Space flash;        // the area: every entry's slot, and the slots of the modules started at this boot that are not
                        // listed any more (switched off or replaced, still running until the restart)
    Space ram;          // the RAM arena
    Space ram_second;   // the second RAM arena (RamSecondArena; ESP8266: RAM in IRAM)
    Space iram;         // ESP8266: the IRAM arena for module code
};

// the module takes arena space: placed (not EntryUnplaced) and not `except` (its new copy replaces it at the next boot,
// the arenas are filled at boot)
bool counted(const Entry& entry, const char* except);

// what the modules take and leave: the flash, the RAM arenas (and the chip's own, arch::arenas)
void spaces(const Directory& dir, bool valid, Spaces& out, const char* except = nullptr);

// --- a package as it arrives ---------------------------------------------------------------------------------------

// its header, then per 4 KiB chunk of the image the chunk's map, (Beken: the chunk's branch list: a count, then the
// words), then the chunk's bytes. A backend's upload derives from it, sets `hooks` (and the branch list storage), and
// gets the header (prepare: place it) and the image bytes in order (bytes: the map of their chunk is `map`, its branch
// list `branches`, `delivered` the image bytes before them, `chunk_left` the bytes of the chunk still to come)
struct PackageReader;

struct PackageHooks {
    const char* (*prepare)(PackageReader& reader);
    const char* (*bytes)(PackageReader& reader, const uint8_t* data, size_t size);
};

struct PackageReader {
    virtual ~PackageReader() = default;

    const PackageHooks* hooks { nullptr };
    PackageHeader header {};
    uint8_t map[MapSize] {};
    uint32_t* branches { nullptr };     // a branch list per chunk when set, up to branches_max words
    uint32_t branches_max { 0 };
    uint32_t branch_count { 0 };
    size_t received { 0 };      // bytes of the request body
    uint32_t body_crc { 0 };    // after the header: maps, branch lists and chunks (PackageHeader::crc32)
    uint32_t delivered { 0 };   // image bytes given to the backend
    size_t fill { 0 };          // of the map / count / branch list
    size_t chunk_left { 0 };    // image bytes of the current chunk still to come
    uint8_t part { 0 };
    bool replace { false };     // ?replace=1
    // placed by the Apply (arch::pools, largest first): RAM offset (RamSecondArena: in the chip's fixed pool), IRAM code
    // offset (ESP8266); the backend's prepare takes them instead of placing itself
    bool planned { false };
    // the Apply's directory in memory (written once at its end): placing goes by it, not by the one in flash
    const Directory* apply_dir { nullptr };
    uint32_t planned_ram { 0 };
    uint32_t planned_iram { 0 };
    const char* error { nullptr };
};

void packageFeed(PackageReader& reader, const uint8_t* data, size_t len);

// the header alone: the package magic and this chip's version, made for this head, the image size
const char* packageCheck(const PackageHeader& header, uint32_t version);

// when the header is in: packageCheck, and the name may go in (installed: a copy is installed, replace); `dir` is
// read for the placing
const char* packageAccept(const PackageReader& reader, uint32_t version, Directory& dir, bool& installed);

// at the end: no error, the whole package came, its CRC
const char* packageComplete(const PackageReader& reader);

// the image written word by word as it arrives, each word relocated by its kind (the chunk's map: delta[kind] added)
// into flash at `slot` (ESP8266, ESP32). The backend's prepare sets slot and delta; `bytes` is wordBytes (ESP32 keeps
// the map behind the image first)
struct WordUpload : PackageReader {
    alignas(4) uint8_t buffer[512];
    uint32_t image_crc { 0 };   // of the words written
    uint32_t slot { 0 };        // flash offset
    uint32_t delta[4] {};

    uint32_t written { 0 };     // image bytes in flash
    size_t buffered { 0 };
    uint32_t word { 0 };
    size_t word_size { 0 };
};

const char* wordBytes(PackageReader& reader, const uint8_t* data, size_t size);

// the end of the package: the rest written, the image read back; its CRC
const char* wordFinish(WordUpload& up, uint32_t& crc);

// --- the guard -----------------------------------------------------------------------------------------------------

// the backend's timer calls it every second (arch::guardTimer)
void guardTick();

namespace arch {
struct GuardState {
    uint32_t running;
    uint32_t booted;
    uint32_t crashes;
};
} // namespace arch

// the guard's count in memory that survives a crash restart (ESP8266 .noinit, ESP32 RTC_NOINIT_ATTR): the backend
// puts one in its section and hands it to guardStateIn() from arch::guardState()
struct GuardSaved {
    uint32_t magic;
    arch::GuardState state;
};

arch::GuardState& guardStateIn(GuardSaved& memory);

// --- what a backend provides ---------------------------------------------------------------------------------------

namespace arch {

// first thing at boot, before the guard reads its state (find the flash area, a key; the chip's own commands)
void setup();

// MODULES.TEST.CRASH: an exception now. Default: a store to 0 (does not fault on every chip)
void crashNow();

uint32_t directoryMagic();

// the module area in flash. start: where the Flash meter starts (ESP8266: 0, the head; ESP32: app0; Beken: the OTA
// partition), lo: the first byte for modules, hi: the directory sector, the last of the area. hi 0 - no area
struct Area {
    uint32_t start;
    uint32_t lo;
    uint32_t hi;
};

Area area();

// the flash as it is stored (Beken: encrypted, a CRC-16 per 32 bytes); erase in whole sectors
bool rawRead(uint32_t offset, void* data, size_t size);
bool rawErase(uint32_t offset, uint32_t size);
bool rawWrite(uint32_t offset, const void* data, size_t size);

// where the image of a module is read as the module runs it (Beken: its logical address), 0 - the entry does not
// match its slot; and those bytes. Default: plain flash (ESP8266, ESP32), its flash offset and rawRead
uint32_t imageAt(const Entry& entry);
bool imageRead(uint32_t at, void* data, size_t size);

// the head image the modules are linked against: the MD5 of its image (ESP8266, ESP32), its GNU build id (Beken)
bool headId(uint8_t out[16]);

// where a module runs in this boot (the image checked already, header in `header`); the core checks the header's
// ranges against it, copies its .data, clears its .bss (and copies the IRAM code), runs its constructors and its entry
struct Runs {
    uint32_t flash_lo;      // the image as data: its header at flash_lo, the .data image, the constructors
    uint32_t flash_hi;
    uint32_t entry_lo;      // its code, the entry inside (ESP32: the instruction view of the slot)
    uint32_t entry_hi;
    uint32_t ram_lo;        // .data and .bss
    uint32_t ram_hi;
    uint32_t iram_lo;       // the header's iram_start..iram_end inside: ESP8266 the IRAM arena, ESP32 the code;
    uint32_t iram_hi;       //  Beken 0, 0 (the header's are 0)
    bool iram_copy;         // ESP8266: the IRAM code copied from iram_load
    bool words;             // 32 bit loads and stores only (ESP8266: the mapped flash, RAM in IRAM)
};

// nullptr - it can run, else why not (ESP32: not relocated for this boot, no RAM)
const char* runs(const Entry& entry, const Header& header, Runs& out);

// before the modules start (ESP32: the arena, relocate the modules whose mapped address moved). Default: nothing
void beforeStart(Directory& dir, bool valid);

// the modules the head's own image brings along, switched on (custom_modules_on: the captive portal), when the board has
// no directory of this head: added to `dir` (written, `valid` then true) as EntryUnplaced, the boot places and starts
// them. ESP8266: an EMC2 file in the tail of the head's .bin. Default: none (ESP32: the initial image holds them placed)
void imageModules(Directory& dir, bool& valid);

// EntryUnplaced: place its RAM (best fit in the arenas the other modules leave, and the RAM of the modules that run
// since this boot) and relocate its image in its slot from the package the board carries; fill the entry's RAM fields
// and CRC (the core clears the flag and writes the directory). Called from the loop or at boot, never from a web
// callback. nullptr - placed, else why not. Default: the chip places the RAM at the install only
const char* place(const Directory& dir, Entry& entry);

// the board can place this module again later without a download (its package is on board): its RAM may be given
// back. Default: no
bool placeable(const Entry& entry);

// flash a module with this image takes in the area, whole sectors (ESP32: the relocation maps behind it, Beken: the
// flash format with a CRC per 32 bytes)
uint32_t slotFor(uint32_t image);

// an upload: its reader with the hooks set (the backend reads its own parameters, ram=iram); the core sets `replace`,
// feeds it, deletes it. uploadFinish: the image written and checked, the entry filled but its name
PackageReader* uploadNew(AsyncWebServerRequest* request);
const char* uploadFinish(PackageReader& upload, Entry& out);

// where the statics (.data + .bss) of the modules go, sized at each Apply, no reserve kept: first the chip's pool at a
// fixed address (Beken: the free data TCM; ESP8266: the IRAM after the head's code, module code first, 32-bit access
// only), largest module first; the rest in one heap block that the next boot takes before the modules start, as big
// as they need (heap: how big it may get, the free heap minus what the head keeps for itself)
struct Pools {
    uint32_t heap;          // bytes the heap block may have
    uint32_t fixed;         // bytes of the fixed pool (RamSecondArena), 0 - none
    bool fixed_words;       // the fixed pool takes 32-bit access only: no module with ModuleByteRam there
    bool code_in_fixed;     // module IRAM code takes the fixed pool from its start (ESP8266)
};

void pools(Pools& out);

// the chip's other arenas (ESP8266: the IRAM arena, the RAM in IRAM) with what the counted modules take. Default: none
void arenas(const Directory& dir, bool valid, Spaces& out, const char* except);

// /modules: the chip's own numbers (its flash chip, ESP8266 the heap reserve) after the common ones
void statusJson(String& out);

// per module: its IRAM code bytes (default 0), the chip's own fields (default none)
uint32_t entryIram(const Entry& entry);
void entryJson(String& out, const Entry& entry);

// MODULES: the chip's lines after the head line, the chip's part of a module's line
void listHead(Print& out);
void listEntry(Print& out, const Entry& entry);

// the modules file the board carries itself (a 4 MB ESP8266 with the full image). Default: none
uint32_t fileSize();
void fileSend(AsyncWebServerRequest* request);

// the head's own page: / without the WebUI module, and /modules.html. false - the chip has none. Default: the one
// WebUI the head embeds (static/index.html.ipp, with the MODULES panel); the ESP8266 head has a small page of its own
bool page(AsyncWebServerRequest* request);

// the last restart was a crash (exception, watchdog)
bool crashRestart();

// the guard's count across restarts (RTC / .noinit memory, flash marks): booted - module code ran past
// MODULES_BOOT_SECONDS in that boot
GuardState& guardState();

// call guardTick() every second from a timer that runs even when loop() waits (ESP8266 SDK timer, a FreeRTOS timer);
// guardTimerStop() from the tick itself, before restartNow()
void guardTimer();
void guardTimerStop();

// restart now, from the guard's timer: no clean shutdown, it needs heap (the restart may come after the tick returns)
void restartNow();

uint32_t freeHeap();
uint32_t failedAllocations();   // default 0: the chip does not count them

// an OTA is running: no module upload then. Default: the web upload
bool otaBusy();

} // namespace arch

} // namespace modules
} // namespace espurna
