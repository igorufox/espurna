/*

PROTOTYPE: the shared core of the module loader (modules_core.h, docs/modules.md)

The directory, the checks and the start of a module image, the state of every module at this boot, the guard, the
package reader and the relocating writer, placing, the /modules web endpoints, the MODULES terminal commands, the start
order and the data modules. What depends on the chip is its backend's (namespace arch).

*/

#include "espurna.h"
#include "modules.h"

#if MODULES_SUPPORT

// the slots of the functions modules provide to each other through the head (modules.h), on every chip
extern "C" {
void* espurna_module_api[static_cast<size_t>(espurna::modules::Api::Count)] {};
}

#endif

#if MODULES_SUPPORT && MODULES_CORE

#include "espurna_ota.h"
#include "modules_core.h"
#include "modules_package.h"
#include "terminal.h"
#include "web.h"
#include "ws.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>

// guard: free heap below this for MODULES_HEAP_SECONDS while modules run -> modules off, restart
#ifndef MODULES_HEAP_MIN
#define MODULES_HEAP_MIN 2048
#endif

#ifndef MODULES_HEAP_SECONDS
#define MODULES_HEAP_SECONDS 5
#endif

// guard: allocations failing in this many seconds in a row -> modules off, restart
#ifndef MODULES_OOM_SECONDS
#define MODULES_OOM_SECONDS 20
#endif

// guard: loop() not passing for this long while the timer runs -> modules off, restart
#ifndef MODULES_LOOP_STALL_SECONDS
#define MODULES_LOOP_STALL_SECONDS 20
#endif

// guard: this many boots in a row crashed with the modules running, before MODULES_BOOT_SECONDS -> modules off
#ifndef MODULES_CRASHES
#define MODULES_CRASHES 3
#endif

#ifndef MODULES_BOOT_SECONDS
#define MODULES_BOOT_SECONDS 60
#endif

namespace espurna {
namespace modules {

// --- small things --------------------------------------------------------------------------------------------------

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t size) {
    crc = ~crc;
    while (size--) {
        crc ^= *data++;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

uint32_t roundUp(uint32_t value, uint32_t to) {
    return package::roundUp(value, to);
}

bool insideRange(uint32_t start, uint32_t end, uint32_t lo, uint32_t hi) {
    return (start <= end) && (start >= lo) && (end <= hi);
}

void jsonField(String& out, const __FlashStringHelper* name, uint32_t value) {
    out += ",\"";
    out += name;
    out += "\":";
    out += String(value, 10);
}

// --- flash ---------------------------------------------------------------------------------------------------------

namespace {

bool inArea(uint32_t offset, uint32_t size) {
    const auto area = arch::area();
    return area.hi && package::inside(offset, size, area.lo, area.hi + SectorSize);
}

} // namespace

bool flashErase(uint32_t offset, uint32_t size) {
    return inArea(offset, size) && ((offset % SectorSize) == 0) && ((size % SectorSize) == 0)
        && arch::rawErase(offset, size);
}

bool flashWrite(uint32_t offset, const void* data, size_t size) {
    return inArea(offset, size) && arch::rawWrite(offset, data, size);
}

bool imageCrc(uint32_t at, uint32_t size, uint32_t& out) {
    alignas(4) uint8_t buffer[256];
    uint32_t crc = 0;
    while (size) {
        const uint32_t part = std::min<uint32_t>(size, sizeof(buffer));
        if (!arch::imageRead(at, buffer, part)) {
            return false;
        }
        crc = crc32Update(crc, buffer, part);
        at += part;
        size -= part;
    }

    out = crc;
    return true;
}

// --- the head ------------------------------------------------------------------------------------------------------

bool hexId(const String& hex, uint8_t out[16]) {
    if (hex.length() != 32) {
        return false;
    }

    for (size_t index = 0; index < 16; ++index) {
        char pair[3] { hex[index * 2], hex[index * 2 + 1], '\0' };
        out[index] = static_cast<uint8_t>(strtoul(pair, nullptr, 16));
    }

    return true;
}

namespace {

bool headBytes(uint8_t out[16]) {
    static uint8_t cached[16];
    static bool done { false };
    if (!done) {
        done = arch::headId(cached);
    }

    if (done) {
        std::memcpy(out, cached, 16);
    }

    return done;
}

String headText() {
    uint8_t id[16];
    if (!headBytes(id)) {
        return String();
    }

    char out[33];
    for (size_t index = 0; index < 16; ++index) {
        snprintf_P(&out[index * 2], 3, PSTR("%02x"), id[index]);
    }

    return String(out);
}

} // namespace

bool sameHead(const uint8_t id[16]) {
    uint8_t own[16];
    return headBytes(own) && (std::memcmp(own, id, 16) == 0);
}

// --- the directory ---------------------------------------------------------------------------------------------

namespace {

// where the directory read last came from, the next write goes into the other copy
uint32_t directory_at { 0 };
uint32_t directory_generation { 0 };

uint32_t directoryCrc(const Directory& dir) {
    return crc32Update(0, reinterpret_cast<const uint8_t*>(&dir), offsetof(Directory, crc));
}

bool directoryNames(const Directory& dir) {
    for (size_t index = 0; index < dir.count; ++index) {
        const auto& name = dir.entries[index].name;
        if (!std::memchr(name, '\0', sizeof(name))) {
            return false;
        }
    }

    return std::memchr(dir.off, '\0', sizeof(dir.off)) != nullptr;
}

bool directoryShape(const Directory& dir) {
    return (dir.magic == arch::directoryMagic()) && (dir.count <= DirectoryEntries) && directoryNames(dir);
}

// the second copy's sector: under the directory, kept out of the module slots (spaces(), the full images)
uint32_t directorySecond() {
    const auto area = arch::area();
    if (!area.hi || (area.hi < (area.lo + SectorSize))) {
        return 0;
    }

    return area.hi - SectorSize;
}

// the one working copy of the directory (about 1 KB with 16 entries): two of them on the 4 KB stack of the ESP8266
// loop next to the caller's own overflowed it (cont_check, 10.0.5.0 2026-11-10)
Directory scratch;

// the heap block of the modules' statics (arch::Pools::heap): taken before anything of ESPurna allocates much
// (modulesRamSetup), as big as the modules placed in it need. With nothing to hold, a probe of it is taken and given
// back: its address is where the Apply places them, the next boot takes the block at that address again
uint8_t* heap_block { nullptr };
uint32_t heap_block_size { 0 };
uint32_t heap_block_base { 0 };

} // namespace

uint32_t heapBlockBase() {
    return heap_block_base;
}

uint32_t heapBlockSize() {
    return heap_block_size;
}

uint32_t heapNeed(const Directory& dir) {
    uint32_t need = 0;
    for (size_t index = 0; index < dir.count; ++index) {
        const auto& entry = dir.entries[index];
        if (entry.ram_size && !(entry.ram_offset & RamSecondArena) && !(entry.flags & EntryUnplaced)) {
            need = std::max<uint32_t>(need, entry.ram_offset + entry.ram_size);
        }
    }

    return roundUp(need, 16);
}

bool readDirectory(Directory& out) {
    const auto area = arch::area();
    if (!area.hi) {
        return false;
    }

    bool found { false };
    const uint32_t places[] { area.hi, directorySecond() };
    for (const auto at : places) {
        if (!at || !arch::rawRead(at, &scratch, sizeof(scratch)) || !directoryShape(scratch)) {
            continue;
        }

        if (scratch.crc != directoryCrc(scratch)) {
            continue;
        }

        if (!found || (scratch.generation > directory_generation)) {
            std::memcpy(&out, &scratch, sizeof(out));
            directory_at = at;
            directory_generation = scratch.generation;
            found = true;
        }
    }

    return found;
}

namespace {

// the bytes in flash at `at` are `data`, read back in small pieces (no second copy on the stack)
bool sameInFlash(uint32_t at, const void* data, size_t size) {
    alignas(4) uint8_t piece[64];
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t done = 0; done < size; done += sizeof(piece)) {
        const size_t part = std::min(sizeof(piece), size - done);
        if (!arch::rawRead(at + done, piece, part) || (std::memcmp(piece, bytes + done, part) != 0)) {
            return false;
        }
    }

    return true;
}

bool writeDirectoryIn(const Directory& dir, bool primary) {
    const auto area = arch::area();
    if (!area.hi) {
        return false;
    }

    // the copy not read last
    const uint32_t second = directorySecond();
    uint32_t at = area.hi;
    if (!primary && second && (directory_at == area.hi)) {
        at = second;
    }

    if (&dir != &scratch) {
        std::memcpy(&scratch, &dir, sizeof(scratch));
    }
    scratch.generation = directory_generation + 1;
    scratch.crc = directoryCrc(scratch);

    if (!flashErase(at, SectorSize) || !flashWrite(at, &scratch, sizeof(scratch)) || !sameInFlash(at, &scratch, sizeof(scratch))) {
        return false;
    }

    directory_at = at;
    directory_generation = scratch.generation;
    return true;
}

} // namespace

bool writeDirectory(const Directory& dir) {
    return writeDirectoryIn(dir, false);
}

// the last sector of the area again, whichever copy is newer (Beken: its boot marks share that sector)
bool writeDirectoryPrimary(const Directory& dir) {
    return writeDirectoryIn(dir, true);
}

namespace {

bool eraseDirectory() {
    const auto area = arch::area();
    if (!area.hi) {
        return false;
    }

    const uint32_t second = directorySecond();
    directory_at = 0;
    directory_generation = 0;
    return flashErase(area.hi, SectorSize) && (!second || flashErase(second, SectorSize));
}

Status status[DirectoryEntries];

} // namespace

Status statusOf(const Entry& entry) {
    for (const auto& out : status) {
        if ((out.state != State::None) && (out.offset == entry.offset) && (out.crc32 == entry.crc32)) {
            return out;
        }
    }

    return Status{};
}

const __FlashStringHelper* stateName(const Status& status) {
    switch (status.state) {
    case State::None:
        return F("not started");
    case State::Skipped:
        return F("skipped (unstable mode)");
    case State::Off:
        return F("off (the guard)");
    case State::Started:
        return F("running");
    case State::Stale:
        return F("stale (another head image)");
    case State::Disabled:
        return F("off");
    case State::Broken:
        break;
    }

    return F("broken");
}

// the guard's decision is in the directory sector (written at once, outside the settings' rotation)
String offReason() {
    Directory dir;
    if (!readDirectory(dir)) {
        return String();
    }

    dir.off[sizeof(dir.off) - 1] = '\0';
    return String(dir.off);
}

// --- a module image ------------------------------------------------------------------------------------------------

bool readHeader(const Entry& entry, Header& out) {
    const auto at = arch::imageAt(entry);
    return at && arch::imageRead(at, &out, sizeof(out));
}

const char* checkImage(const Entry& entry, Header& out) {
    if (!readHeader(entry, out)) {
        return PSTR("header read");
    }

    if ((out.magic != HeaderMagic) || (out.version != HeaderVersion)) {
        return PSTR("no module header");
    }

    if (!sameHead(out.head_md5)) {
        return PSTR("stale");
    }

    uint32_t crc;
    if (!imageCrc(arch::imageAt(entry), entry.size, crc) || (crc != entry.crc32)) {
        return PSTR("crc");
    }

    return nullptr;
}

namespace {

// the one WebUI (code/html, `gulp build`, with the MODULES panel); web.cpp leaves its own out with MODULES_SUPPORT.
// Only arch::page's default uses it: on the ESP8266 (a page of its own) the linker drops both
#include "static/index.html.ipp"


void offWrite(const char* reason) {
    Directory dir;
    if (!readDirectory(dir)) {
        return;
    }

    std::strncpy(dir.off, reason, sizeof(dir.off) - 1);
    dir.off[sizeof(dir.off) - 1] = '\0';
    writeDirectory(dir);
}

void modulesOff(const String& reason) {
    offWrite(reason.c_str());
    DEBUG_MSG_P(PSTR("[MODULES] Off: %s\n"), reason.c_str());
}

void modulesOffClear() {
    Directory dir;
    if (readDirectory(dir) && dir.off[0]) {
        dir.off[0] = '\0';
        writeDirectory(dir);
    }
}

// what started this boot: its data (a data module), or its code runs
struct Started {
    bool code { false };
    const uint8_t* data { nullptr };
    size_t size { 0 };
};

// copies with 32 bit loads and stores only
void copyWords(uint32_t to, uint32_t from, uint32_t size) {
    const auto* src = reinterpret_cast<const volatile uint32_t*>(from);
    auto* dst = reinterpret_cast<volatile uint32_t*>(to);
    for (uint32_t index = 0; index < (size / 4); ++index) {
        dst[index] = src[index];
    }
}

void zeroWords(uint32_t to, uint32_t size) {
    auto* dst = reinterpret_cast<volatile uint32_t*>(to);
    for (uint32_t index = 0; index < (size / 4); ++index) {
        dst[index] = 0;
    }
}

// the image checked, its ranges against where the backend says it runs; its data copied, its bss cleared, its IRAM
// code copied, its constructors and its entry run. nullptr - started; PSTR("stale") - made for another head; else why
// not. A data module (header.entry 0) gives its bytes
void guardArm();

const char* run(const Entry& entry, Started& out) {
    Header header;
    const auto* error = checkImage(entry, header);
    if (error) {
        return error;
    }

    arch::Runs runs {};
    error = arch::runs(entry, header, runs);
    if (error) {
        return error;
    }

    // data module: only its bytes, nothing to copy or call
    if (!header.entry) {
        if ((header.flash_start != runs.flash_lo) || (header.flash_end != runs.flash_hi)
         || (entry.size < sizeof(Header)) || (header.data_end > (entry.size - sizeof(Header))))
        {
            return PSTR("ranges");
        }

        out.code = false;
        out.data = reinterpret_cast<const uint8_t*>(runs.flash_lo + sizeof(Header));
        out.size = header.data_end;
        return nullptr;
    }

    const bool has_ram = (header.data_end != header.data_start) || (header.bss_end != header.bss_start);
    const uint32_t data_size = header.data_end - header.data_start;
    const uint32_t iram_size = header.iram_end - header.iram_start;
    const uint32_t entry_at = header.entry & ~1u;   // Thumb: the low bit says so

    if ((header.flash_start != runs.flash_lo)
     || !insideRange(header.flash_start, header.flash_end, runs.flash_lo, runs.flash_hi)
     || (has_ram && !insideRange(header.data_start, header.data_end, runs.ram_lo, runs.ram_hi))
     || (has_ram && !insideRange(header.bss_start, header.bss_end, runs.ram_lo, runs.ram_hi))
     || !insideRange(header.iram_start, header.iram_end, runs.iram_lo, runs.iram_hi)
     || (runs.iram_copy && !insideRange(header.iram_load, header.iram_load + iram_size, runs.flash_lo, runs.flash_hi))
     || !insideRange(header.data_load, header.data_load + data_size, runs.flash_lo, runs.flash_hi)
     || !insideRange(header.init_start, header.init_end, runs.flash_lo, runs.flash_hi)
     || !insideRange(entry_at, entry_at + 2, runs.entry_lo, runs.entry_hi))
    {
        return PSTR("ranges");
    }

    // .data + .rodata, then .bss
    if (runs.words) {
        copyWords(header.data_start, header.data_load, data_size);
        zeroWords(header.bss_start, header.bss_end - header.bss_start);
    } else {
        if (data_size) {
            std::memcpy(reinterpret_cast<void*>(header.data_start), reinterpret_cast<const void*>(header.data_load),
                data_size);
        }
        if (header.bss_end > header.bss_start) {
            std::memset(reinterpret_cast<void*>(header.bss_start), 0, header.bss_end - header.bss_start);
        }
    }

    // IRAM takes 32 bit stores only, words from the mapped flash
    if (runs.iram_copy) {
        copyWords(header.iram_start, header.iram_load, iram_size);
    }

    // the guard watches from the first instruction of the module on: a crash in its constructors or its entry()
    // counts, otherwise the boot crash loop never ends
    guardArm();

    using Func = void(*)();
    for (auto* ptr = reinterpret_cast<const Func*>(header.init_start);
            ptr != reinterpret_cast<const Func*>(header.init_end); ++ptr)
    {
        (*ptr)();
    }

    reinterpret_cast<Func>(header.entry)();
    out.code = true;
    return nullptr;
}

// --- dependencies: the modules a module needs, by name (Header::needs, comma separated) ----------------------------

struct Requires {
    char names[4][16] {};
    size_t count { 0 };
};

Requires requiresOf(const Entry& entry) {
    Requires out;
    Header header;
    // no module header in the slot (one of the head's image not placed yet): it needs nothing we know of
    if (!readHeader(entry, header) || (header.magic != HeaderMagic)) {
        return out;
    }

    char list[sizeof(header.needs) + 1];
    std::memcpy(list, header.needs, sizeof(header.needs));
    list[sizeof(header.needs)] = '\0';

    char* item = list;
    while (item && *item && (out.count < 4)) {
        char* next = std::strchr(item, ',');
        if (next) {
            *next++ = '\0';
        }
        while (*item == ' ') {
            ++item;
        }
        if (*item) {
            std::strncpy(out.names[out.count], item, sizeof(out.names[out.count]) - 1);
            ++out.count;
        }
        item = next;
    }

    return out;
}

bool needs(const Entry& entry, const char* name) {
    const auto list = requiresOf(entry);
    for (size_t index = 0; index < list.count; ++index) {
        if (std::strncmp(list.names[index], name, sizeof(list.names[index])) == 0) {
            return true;
        }
    }
    return false;
}

const Entry* findEntry(const Directory& dir, const char* name) {
    for (size_t index = 0; index < dir.count; ++index) {
        if (std::strncmp(dir.entries[index].name, name, sizeof(dir.entries[index].name)) == 0) {
            return &dir.entries[index];
        }
    }
    return nullptr;
}

// another installed module needs this one (switched on ones only, or all)
const Entry* neededBy(const Directory& dir, const char* name, bool only_on) {
    for (size_t index = 0; index < dir.count; ++index) {
        const auto& other = dir.entries[index];
        if (only_on && (other.flags & EntryDisabled)) {
            continue;
        }
        if (needs(other, name)) {
            return &other;
        }
    }
    return nullptr;
}

bool removeEntry(const String& name, const char*& error) {
    Directory dir;
    if (!readDirectory(dir)) {
        error = PSTR("no modules");
        return false;
    }

    if (neededBy(dir, name.c_str(), false)) {
        error = PSTR("another installed module needs it");
        return false;
    }

    // Remove takes out a module that is off (MODULES.DISABLE / the panel's switch first)
    const auto* entry = findEntry(dir, name.c_str());
    if (entry && !(entry->flags & EntryDisabled)) {
        error = PSTR("switch it off first");
        return false;
    }

    size_t out = 0;
    for (size_t index = 0; index < dir.count; ++index) {
        if (!name.equals(dir.entries[index].name)) {
            dir.entries[out++] = dir.entries[index];
        }
    }

    if (out == dir.count) {
        error = PSTR("not installed");
        return false;
    }

    dir.count = out;
    if (!writeDirectory(dir)) {
        error = PSTR("directory write");
        return false;
    }

    return true;
}

// --- placing -------------------------------------------------------------------------------------------------------

// flash of the modules started at this boot: a module switched off keeps running from it until the restart, so a
// module switched on before the restart (the same one with its RAM moved, too) must be placed elsewhere
struct BootPiece {
    uint32_t offset;
    uint32_t size;
};

BootPiece boot_flash[DirectoryEntries];
size_t boot_flash_count { 0 };

void flashPieces(const Directory& dir, bool valid, Space& flash) {
    for (size_t index = 0; valid && (index < dir.count); ++index) {
        flash.add(dir.entries[index].offset, dir.entries[index].slot);
    }

    for (size_t piece = 0; piece < boot_flash_count; ++piece) {
        bool listed = false;
        for (size_t index = 0; valid && (index < dir.count); ++index) {
            listed = listed || (dir.entries[index].offset == boot_flash[piece].offset);
        }
        if (!listed) {
            flash.add(boot_flash[piece].offset, boot_flash[piece].size);
        }
    }
}

} // namespace

bool startedAt(uint32_t offset) {
    for (size_t piece = 0; piece < boot_flash_count; ++piece) {
        if (boot_flash[piece].offset == offset) {
            return true;
        }
    }

    return false;
}

size_t startedCount() {
    return boot_flash_count;
}

void Space::add(uint32_t from, uint32_t size) {
    if (!size || (count >= (DirectoryEntries * 2))) {
        return;
    }
    size_t pos = count++;
    while (pos && (start[pos - 1] > from)) {
        start[pos] = start[pos - 1];
        end[pos] = end[pos - 1];
        --pos;
    }
    start[pos] = from;
    end[pos] = from + size;
}

uint32_t Space::gapStart(size_t index) const {
    return index ? std::max(lo, end[index - 1]) : lo;
}

uint32_t Space::gapEnd(size_t index) const {
    return (index < count) ? std::min(hi, start[index]) : hi;
}

uint32_t Space::gap(size_t index) const {
    const auto a = gapStart(index);
    const auto b = gapEnd(index);
    return (b > a) ? (b - a) : 0;
}

uint32_t Space::free() const {
    uint32_t out = 0;
    for (size_t index = 0; index <= count; ++index) {
        out += gap(index);
    }
    return out;
}

uint32_t Space::largest() const {
    uint32_t out = 0;
    for (size_t index = 0; index <= count; ++index) {
        out = std::max(out, gap(index));
    }
    return out;
}

bool Space::place(uint32_t size, bool top, uint32_t& out) const {
    bool found = false;
    uint32_t best = 0;
    for (size_t index = 0; index <= count; ++index) {
        const auto length = gap(index);
        if ((length >= size) && (!found || (length < best) || (top && (length == best)))) {
            out = top ? (gapEnd(index) - size) : gapStart(index);
            best = length;
            found = true;
        }
    }
    return found;
}

bool counted(const Entry& entry, const char* except) {
    // off, its RAM given back (EntryUnplaced): no arena counts it
    if (entry.flags & EntryUnplaced) {
        return false;
    }

    return !except || (std::strncmp(entry.name, except, sizeof(entry.name)) != 0);
}

void spaces(const Directory& dir, bool valid, Spaces& out, const char* except) {
    const auto area = arch::area();
    out.flash.lo = area.lo;
    // the directory's second copy under it (DirectorySectors)
    out.flash.hi = (area.hi >= (area.lo + SectorSize)) ? (area.hi - SectorSize) : area.lo;
    flashPieces(dir, valid, out.flash);

    arch::Pools pools {};
    arch::pools(pools);
    out.ram.lo = 0;
    out.ram.hi = pools.heap;
    for (size_t index = 0; valid && (index < dir.count); ++index) {
        const auto& entry = dir.entries[index];
        if (!counted(entry, except)) {
            continue;
        }
        if (entry.ram_offset & RamSecondArena) {
            out.ram_second.add(entry.ram_offset & ~RamSecondArena, entry.ram_size);
        } else {
            out.ram.add(entry.ram_offset, entry.ram_size);
        }
    }

    arch::arenas(dir, valid, out, except);
}

// --- the guard's count ---------------------------------------------------------------------------------------------

namespace {

constexpr uint32_t GuardMagic { 0x34474D45 }; // "EMG4"

} // namespace

arch::GuardState& guardStateIn(GuardSaved& memory) {
    if (memory.magic != GuardMagic) {
        memory.magic = GuardMagic;
        memory.state = arch::GuardState{0, 0, 0};
    }

    return memory.state;
}

// --- a package as it arrives ---------------------------------------------------------------------------------------

namespace {

// PackageReader::part
constexpr uint8_t PartMap { 0 };
constexpr uint8_t PartCount { 1 };
constexpr uint8_t PartBranches { 2 };
constexpr uint8_t PartChunk { 3 };

const char* installCheck(const Directory& dir, const char* name, bool replace, bool& installed) {
    installed = false;
    for (size_t index = 0; index < dir.count; ++index) {
        if (std::strncmp(dir.entries[index].name, name, sizeof(dir.entries[index].name)) == 0) {
            if (!replace) {
                return PSTR("installed already (MODULES.REMOVE first)");
            }
            installed = true;
        }
    }

    if (!installed && (dir.count >= DirectoryEntries)) {
        return PSTR("directory full");
    }

    return nullptr;
}

} // namespace

void packageFeed(PackageReader& reader, const uint8_t* data, size_t len) {
    const auto& hooks = *reader.hooks;
    while (len && !reader.error) {
        // the package header
        if (reader.received < sizeof(PackageHeader)) {
            const size_t part = std::min(len, sizeof(PackageHeader) - reader.received);
            std::memcpy(reinterpret_cast<uint8_t*>(&reader.header) + reader.received, data, part);
            reader.received += part;
            data += part;
            len -= part;
            if (reader.received == sizeof(PackageHeader)) {
                reader.error = hooks.prepare(reader);
            }
            continue;
        }

        if (reader.delivered >= reader.header.image_size) {
            reader.error = PSTR("more data than the image");
            return;
        }

        // the chunk's bytes
        if (reader.part == PartChunk) {
            const size_t part = std::min(len, reader.chunk_left);
            reader.body_crc = crc32Update(reader.body_crc, data, part);
            reader.error = hooks.bytes(reader, data, part);
            reader.delivered += part;
            reader.chunk_left -= part;
            reader.received += part;
            data += part;
            len -= part;
            if (!reader.chunk_left) {
                reader.part = PartMap;
            }
            continue;
        }

        // the map of the next chunk, its branch count, its branch list
        uint8_t* target { reinterpret_cast<uint8_t*>(reader.map) };
        size_t want { MapSize };
        if (reader.part == PartCount) {
            target = reinterpret_cast<uint8_t*>(&reader.branch_count);
            want = sizeof(reader.branch_count);
        } else if (reader.part == PartBranches) {
            target = reinterpret_cast<uint8_t*>(reader.branches);
            want = reader.branch_count * 4;
        }

        const size_t part = std::min(len, want - reader.fill);
        std::memcpy(target + reader.fill, data, part);
        reader.body_crc = crc32Update(reader.body_crc, data, part);
        reader.fill += part;
        reader.received += part;
        data += part;
        len -= part;
        if (reader.fill < want) {
            continue;
        }

        reader.fill = 0;
        if ((reader.part == PartMap) && reader.branches) {
            reader.part = PartCount;
            continue;
        }

        if (reader.part == PartCount) {
            if (reader.branch_count > reader.branches_max) {
                reader.error = PSTR("too many branches in a chunk");
                return;
            }
            if (reader.branch_count) {
                reader.part = PartBranches;
                continue;
            }
        }

        reader.chunk_left = std::min<size_t>(ChunkSize, reader.header.image_size - reader.delivered);
        reader.part = PartChunk;
    }
}

const char* packageCheck(const PackageHeader& header, uint32_t version) {
    if ((header.magic != PackageMagic) || (header.version != version)) {
        return PSTR("not a module package of this chip");
    }

    if (!sameHead(header.head_md5)) {
        return PSTR("the module was built for another head image");
    }

    // nothing larger than the module area itself: a slot rounded from a huge size can never be placed
    const auto area = arch::area();
    const uint32_t area_size = (area.hi && (area.hi >= area.lo)) ? (area.hi + SectorSize - area.lo) : 0;
    switch (package::sizesCheck(header.image_size, header.ram_size, header.iram_size, area_size)) {
    case package::Sizes::Ok:
        break;
    case package::Sizes::BadImage:
        return PSTR("bad image size");
    case package::Sizes::ImageTooLarge:
        return PSTR("image larger than the module area");
    case package::Sizes::BadRam:
        return PSTR("bad RAM size");
    }

    return nullptr;
}

const char* packageAccept(const PackageReader& reader, uint32_t version, Directory& dir, bool& installed) {
    const auto* error = packageCheck(reader.header, version);
    if (error) {
        return error;
    }

    if (!readDirectory(dir)) {
        std::memset(&dir, 0, sizeof(dir));
    }

    return installCheck(dir, reader.header.name, reader.replace, installed);
}

const char* packageComplete(const PackageReader& reader) {
    if (reader.error) {
        return reader.error;
    }

    if ((reader.received < sizeof(PackageHeader)) || (reader.delivered != reader.header.image_size)) {
        return PSTR("short upload");
    }

    if (reader.body_crc != reader.header.crc32) {
        return PSTR("package crc");
    }

    return nullptr;
}

// --- the relocating writer ---------------------------------------------------------------------------------------------

namespace {

bool wordFlush(WordUpload& up) {
    if (!up.buffered) {
        return true;
    }

    // chunks are 4 KiB and sector aligned, the buffer divides a sector: a flush never crosses a sector
    const uint32_t address = up.slot + up.written;
    if (((address % SectorSize) == 0) && !flashErase(address, SectorSize)) {
        return false;
    }

    if (!flashWrite(address, up.buffer, up.buffered)) {
        return false;
    }

    up.image_crc = crc32Update(up.image_crc, up.buffer, up.buffered);
    up.written += up.buffered;
    up.buffered = 0;
    return true;
}

// one image word, in chunk order
bool wordPut(WordUpload& up, uint32_t value) {
    const size_t index = ((up.written + up.buffered) % ChunkSize) / 4;
    const uint8_t kind = (up.map[index >> 2] >> ((index & 3) * 2)) & 3;
    value += up.delta[kind];

    std::memcpy(&up.buffer[up.buffered], &value, 4);
    up.buffered += 4;
    if (up.buffered == sizeof(up.buffer)) {
        return wordFlush(up);
    }

    return true;
}

} // namespace

const char* wordBytes(PackageReader& reader, const uint8_t* data, size_t size) {
    auto& up = static_cast<WordUpload&>(reader);
    for (size_t index = 0; index < size; ++index) {
        up.word |= static_cast<uint32_t>(data[index]) << (8 * up.word_size);
        if (++up.word_size == 4) {
            if (!wordPut(up, up.word)) {
                return PSTR("flash write");
            }
            up.word = 0;
            up.word_size = 0;
        }
    }

    return nullptr;
}

const char* wordFinish(WordUpload& up, uint32_t& crc) {
    const auto* error = packageComplete(up);
    if (error) {
        return error;
    }

    if (!wordFlush(up) || (up.written != up.header.image_size)) {
        return PSTR("flash write");
    }

    if (!imageCrc(up.slot, up.written, crc) || (crc != up.image_crc)) {
        return PSTR("verify");
    }

    return nullptr;
}

// --- the defaults of the contract ----------------------------------------------------------------------------------

namespace arch {

// plain flash: the image is read where it is stored
__attribute__((weak)) uint32_t imageAt(const Entry& entry) {
    return entry.offset;
}

__attribute__((weak)) bool imageRead(uint32_t at, void* data, size_t size) {
    return rawRead(at, data, size);
}

__attribute__((weak)) void crashNow() {
    *reinterpret_cast<volatile uint32_t*>(0) = 0;
}

__attribute__((weak)) void beforeStart(Directory&, bool) {
}

__attribute__((weak)) void imageModules(Directory&, bool&) {
}

// a backend that places the RAM at the install only: nothing given back, nothing to place
__attribute__((weak)) const char* place(const Directory&, Entry&) {
    return PSTR("this chip places the RAM at the install only");
}

__attribute__((weak)) bool placeable(const Entry&) {
    return false;
}

__attribute__((weak)) void arenas(const Directory&, bool, Spaces&, const char*) {
}

__attribute__((weak)) uint32_t entryIram(const Entry&) {
    return 0;
}

__attribute__((weak)) void entryJson(String&, const Entry&) {
}

__attribute__((weak)) uint32_t fileSize() {
    return 0;
}

__attribute__((weak)) void fileSend(AsyncWebServerRequest* request) {
    request->send(404, String(F("text/plain")), String(F("ERROR: no modules file in flash")));
}

// gzip, cached by its Last-Modified
__attribute__((weak)) bool page(AsyncWebServerRequest* request) {
    const auto* modified = request->getHeader(String(F("If-Modified-Since")));
    if (modified && modified->value().equals(FPSTR(webui_last_modified))) {
        request->send(304);
        return true;
    }

    auto* response = request->beginResponse_P(200, String(F("text/html")), webui_data, sizeof(webui_data));
    response->addHeader(String(F("Content-Encoding")), String(FPSTR(webui_content_encoding)));
    response->addHeader(String(F("Last-Modified")), String(FPSTR(webui_last_modified)));
    request->send(response);
    return true;
}

__attribute__((weak)) uint32_t failedAllocations() {
    return 0;
}

__attribute__((weak)) bool otaBusy() {
    return otaWebUploading();
}

} // namespace arch

namespace {

// --- the guard: modules off after crashes at boot, a heap shortage, failing allocations or a stalled loop --------

// MODULES.TEST.LOWHEAP: the guard sees no free heap
bool test_low_heap { false };

void guardBoot() {
    auto& guard = arch::guardState();

    // a boot crash: the modules ran, but not past their first MODULES_BOOT_SECONDS. Boots without module code (the
    // unstable mode after the crashes, its exit restart) keep the count, so the crash loop ends here and not in
    // ESPurna's unstable mode every 10 minutes; modules running past their boot clear it
    if (arch::crashRestart() && guard.running && !guard.booted) {
        ++guard.crashes;
    } else if (guard.running && guard.booted) {
        guard.crashes = 0;
    }
    guard.running = 0;
    guard.booted = 0;

    if (guard.crashes >= MODULES_CRASHES) {
        modulesOff(String(F("the boot crashed ")) + String(guard.crashes, 10)
            + F(" times in a row with the modules running"));
        guard.crashes = 0;
    }
}

// The guard ticks on a timer, not in loop(): with the heap gone the loop can sit in a wait that yields (the
// watchdog is fed, nothing restarts the board; 10.0.5.0 hung like that, 2026-11-09). Every second:
// - free heap below MODULES_HEAP_MIN for MODULES_HEAP_SECONDS,
// - allocations failing in MODULES_OOM_SECONDS seconds in a row,
// - loop() not passing for MODULES_LOOP_STALL_SECONDS
// -> the modules are off (the reason in the directory) and the board restarts at once: a clean restart needs heap
uint32_t loop_passes { 0 };

void guardLoop() {
    ++loop_passes;
}

void guardDecide(const char* reason) {
    arch::guardTimerStop();
    offWrite(reason);
    arch::restartNow();
}

} // namespace

void guardTick() {
    static uint32_t seconds { 0 };
    static uint32_t low { 0 };
    static uint32_t oom_seconds { 0 };
    static uint32_t oom_last { 0 };
    static uint32_t passes_last { 0 };
    static uint32_t stalled { 0 };
    ++seconds;

    auto& guard = arch::guardState();
    if (!guard.booted && (seconds >= MODULES_BOOT_SECONDS)) {
        guard.booted = 1;
        guard.crashes = 0;
    }

    char reason[96];

    const auto free = test_low_heap ? 0 : arch::freeHeap();
    low = (free < MODULES_HEAP_MIN) ? (low + 1) : 0;
    if (low >= MODULES_HEAP_SECONDS) {
        snprintf_P(reason, sizeof(reason), PSTR("free heap %u bytes for %u s with the modules running"), free, low);
        guardDecide(reason);
        return;
    }

    const auto oom = arch::failedAllocations();
    oom_seconds = (oom != oom_last) ? (oom_seconds + 1) : 0;
    oom_last = oom;
    if (oom_seconds >= MODULES_OOM_SECONDS) {
        snprintf_P(reason, sizeof(reason), PSTR("allocations failing for %u s (%u in all), free heap %u"),
            oom_seconds, oom, free);
        guardDecide(reason);
        return;
    }

    stalled = (loop_passes == passes_last) ? (stalled + 1) : 0;
    passes_last = loop_passes;
    if (stalled >= MODULES_LOOP_STALL_SECONDS) {
        snprintf_P(reason, sizeof(reason), PSTR("the loop stalled for %u s, free heap %u"), stalled, free);
        guardDecide(reason);
    }
}

namespace {

// --- started modules -----------------------------------------------------------------------------------------------

bool running { false };     // code of a module runs (data modules do not count)
bool guard_armed { false };

// data of a started data module, e.g. the WebUI
struct Data {
    char name[16];
    const uint8_t* data;
    size_t size;
};

Data data[DirectoryEntries];
size_t data_count { 0 };

void guardArm();

Status* statusSlot(const Entry& entry) {
    for (auto& out : status) {
        if ((out.state != State::None) && (out.offset == entry.offset) && (out.crc32 == entry.crc32)) {
            return &out;
        }
    }
    for (auto& out : status) {
        if (out.state == State::None) {
            return &out;
        }
    }
    return nullptr;
}

// a copy of this module runs (another one, replaced before the restart, too)
bool runningByName(const char* name) {
    for (const auto& out : status) {
        if ((out.state == State::Started) && (std::strncmp(out.name, name, sizeof(out.name)) == 0)) {
            return true;
        }
    }
    return false;
}

// EntryUnplaced: its RAM now (the directory is written); nullptr - ready to start
const char* placeNow(Directory& dir, Entry& entry) {
    if (!(entry.flags & EntryUnplaced)) {
        return nullptr;
    }

    // a crash while placing (the relocation) counts for the guard as well
    if (entry.ram_size) {
        guardArm();
    }
    const auto* error = arch::place(dir, entry);
    if (error) {
        return error;
    }

    entry.flags &= ~EntryUnplaced;
    if (!writeDirectory(dir)) {
        return PSTR("directory write");
    }
    DEBUG_MSG_P(PSTR("[MODULES] %s placed (RAM %u)\n"), entry.name, entry.ram_size);
    return nullptr;
}

// off and not running: its RAM back, when the board can place it again. true - the entry changed
bool giveBack(Entry& entry) {
    if ((entry.flags & EntryUnplaced) || !arch::placeable(entry)) {
        return false;
    }

    entry.flags |= EntryUnplaced;
    return true;
}

// start one module: at boot, or when it is switched on (from the loop, never from a web or TCP callback)
const char* startEntry(const Entry& entry) {
    auto* slot = statusSlot(entry);
    if (!slot) {
        return PSTR("no status slot");
    }

    const uint32_t heap_before = arch::freeHeap();
    Started started;
    // run() arms the guard right before the module's first code (its constructors, entry())
    const auto* error = run(entry, started);
    if (error) {
        const bool stale = (strcmp_P("stale", error) == 0);
        *slot = Status{stale ? State::Stale : State::Broken, error, entry.offset, entry.crc32};
        std::memcpy(slot->name, entry.name, sizeof(slot->name));
        DEBUG_MSG_P(PSTR("[MODULES] %s not started: %s\n"), entry.name, error);
        return error;
    }

    const uint32_t heap_after = arch::freeHeap();
    *slot = Status{State::Started, nullptr, entry.offset, entry.crc32,
        (heap_before > heap_after) ? (heap_before - heap_after) : 0};
    std::memcpy(slot->name, entry.name, sizeof(slot->name));
    if (boot_flash_count < DirectoryEntries) {
        boot_flash[boot_flash_count++] = BootPiece{entry.offset, entry.slot};
    }
    if (started.code) {
        running = true;
        guardArm();
    } else if (data_count < DirectoryEntries) {
        auto& out = data[data_count++];
        std::memcpy(out.name, entry.name, sizeof(out.name));
        out.data = started.data;
        out.size = started.size;
    }
    DEBUG_MSG_P(PSTR("[MODULES] %s started (0x%05x, %u bytes)\n"), entry.name, entry.offset, entry.size);
    return nullptr;
}

// modules switched on (or uploaded) while the board runs: started by the loop
char pending[DirectoryEntries][16];
size_t pending_count { 0 };

void startLater(const char* name) {
    for (size_t index = 0; index < pending_count; ++index) {
        if (std::strncmp(pending[index], name, sizeof(pending[index])) == 0) {
            return;
        }
    }
    if (pending_count < DirectoryEntries) {
        std::strncpy(pending[pending_count], name, sizeof(pending[pending_count]) - 1);
        pending[pending_count][sizeof(pending[pending_count]) - 1] = '\0';
        ++pending_count;
    }
}

void pendingLoop() {
    if (!pending_count) {
        return;
    }
    if (offReason().length()) {
        pending_count = 0;
        return;
    }

    char name[16];
    std::memcpy(name, pending[0], sizeof(name));
    --pending_count;
    std::memmove(&pending[0], &pending[1], pending_count * sizeof(pending[0]));

    Directory dir;
    if (!readDirectory(dir)) {
        return;
    }

    for (size_t index = 0; index < dir.count; ++index) {
        const auto& entry = dir.entries[index];
        if (std::strncmp(entry.name, name, sizeof(entry.name)) != 0) {
            continue;
        }
        if ((entry.flags & EntryDisabled) || runningByName(entry.name)) {
            return;
        }

        // what it needs runs first: one still queued - this one waits behind it; one missing, off or failed - it
        // does not start
        const auto list = requiresOf(entry);
        for (size_t need = 0; need < list.count; ++need) {
            if (runningByName(list.names[need])) {
                continue;
            }
            bool queued = false;
            for (size_t other = 0; other < pending_count; ++other) {
                queued = queued || (std::strncmp(pending[other], list.names[need], sizeof(pending[other])) == 0);
            }
            if (queued) {
                startLater(entry.name);
                return;
            }
            if (auto* slot = statusSlot(entry)) {
                *slot = Status{State::Broken, PSTR("needs a module that is not running"), entry.offset, entry.crc32};
                std::memcpy(slot->name, entry.name, sizeof(slot->name));
            }
            DEBUG_MSG_P(PSTR("[MODULES] %s not started: needs %s\n"), entry.name, list.names[need]);
            return;
        }

        arch::beforeStart(dir, true);
        auto& target = dir.entries[index];
        const auto* error = placeNow(dir, target);
        if (error) {
            if (auto* slot = statusSlot(target)) {
                *slot = Status{State::Broken, error, target.offset, target.crc32};
                std::memcpy(slot->name, target.name, sizeof(slot->name));
            }
            DEBUG_MSG_P(PSTR("[MODULES] %s not started: %s\n"), target.name, error);
            return;
        }
        startEntry(target);
        return;
    }
}

// on: the flag off, the module starts now (unless a copy of it runs: then at the restart). off: the flag on, its code
// keeps running until the restart, its flash stays
// switch on this one and what it needs (installed ones only), the deepest first; queued for the loop in that order
const char* enableTree(Directory& dir, const char* name, size_t depth, bool& changed) {
    if (depth > 4) {
        return PSTR("dependencies too deep (a loop?)");
    }

    auto* entry = const_cast<Entry*>(findEntry(dir, name));
    if (!entry) {
        return PSTR("needs a module that is not installed");
    }

    const auto list = requiresOf(*entry);
    for (size_t index = 0; index < list.count; ++index) {
        const auto* error = enableTree(dir, list.names[index], depth + 1, changed);
        if (error) {
            return error;
        }
    }

    if (entry->flags & EntryDisabled) {
        entry->flags &= ~EntryDisabled;
        changed = true;
    }
    if (!runningByName(entry->name)) {
        startLater(entry->name);
    }
    return nullptr;
}

// on: the flag off for it and what it needs, they start now (unless a copy runs: then at the restart). off: refused
// while a switched-on module needs it; its code keeps running until the restart, its flash stays
const char* setEnabled(const String& name, bool on, String& text) {
    Directory dir;
    if (!readDirectory(dir)) {
        return PSTR("no modules");
    }

    const auto* entry = findEntry(dir, name.c_str());
    if (!entry) {
        return PSTR("not installed");
    }

    if (on) {
        bool changed = false;
        const auto* error = enableTree(dir, entry->name, 0, changed);
        if (error) {
            return error;
        }
        if (changed && !writeDirectory(dir)) {
            return PSTR("directory write");
        }
        text = runningByName(entry->name) ? F("OK, on") : F("OK, starting");
        return nullptr;
    }

    if (neededBy(dir, entry->name, true)) {
        return PSTR("a switched on module needs it, switch that off first");
    }

    const_cast<Entry*>(entry)->flags |= EntryDisabled;
    if (!runningByName(entry->name)) {
        giveBack(*const_cast<Entry*>(entry));
    }
    if (!writeDirectory(dir)) {
        return PSTR("directory write");
    }
    text = runningByName(entry->name) ? F("OK, off after restart") : F("OK, off");
    return nullptr;
}

// --- upload --------------------------------------------------------------------------------------------------------

PackageReader* upload { nullptr };
AsyncWebServerRequest* owner { nullptr };

void release() {
    delete upload;
    upload = nullptr;
    owner = nullptr;
}

void answer(AsyncWebServerRequest* request, int code, const String& text) {
    auto* response = request->beginResponse(code, String(F("text/plain")), text);
    response->addHeader(String(F("Connection")), String(F("close")));
    request->send(response);
}

bool authenticated(AsyncWebServerRequest* request) {
    if (!webAuthenticate(request)) {
        request->requestAuthentication(systemHostname().c_str());
        return false;
    }

    return true;
}

// the directory gets the entry once the backend wrote and checked the module: a new one at the end, a new copy of an
// installed one (replace) in its place
const char* addEntry(const Entry& entry) {
    Directory dir;
    if (!readDirectory(dir)) {
        std::memset(&dir, 0, sizeof(dir));
        dir.magic = arch::directoryMagic();
    }

    size_t index = dir.count;
    for (size_t other = 0; other < dir.count; ++other) {
        if (std::strncmp(dir.entries[other].name, entry.name, sizeof(entry.name)) == 0) {
            index = other;
        }
    }

    if (index >= DirectoryEntries) {
        return PSTR("directory full");
    }

    dir.entries[index] = entry;
    if (index == dir.count) {
        ++dir.count;
    }

    return writeDirectory(dir) ? nullptr : PSTR("directory write");
}

void onUploadDone(AsyncWebServerRequest* request) {
    if (!authenticated(request)) {
        return;
    }

    if (!upload || (owner != request)) {
        answer(request, 400, F("ERROR: no module upload in this request"));
        return;
    }

    Entry entry;
    std::memset(&entry, 0, sizeof(entry));
    const auto* error = arch::uploadFinish(*upload, entry);
    if (!error) {
        std::memcpy(entry.name, upload->header.name, sizeof(entry.name));
        entry.name[sizeof(entry.name) - 1] = '\0';
        error = addEntry(entry);
    }

    String text;
    if (error) {
        text = String(F("ERROR: ")) + FPSTR(error);
        answer(request, 400, text);
    } else if (!runningByName(entry.name) && !offReason().length()) {
        // it and what it needs (installed ones) start now, the deepest first
        String started;
        const auto* problem = setEnabled(String(entry.name), true, started);
        text = String(F("OK, ")) + entry.name + F(" at 0x") + String(entry.offset, 16)
            + (problem ? String(F(", installed; ")) + FPSTR(problem) : String(F(", starting")));
        answer(request, 200, text);
    } else {
        text = String(F("OK, ")) + entry.name + F(" at 0x") + String(entry.offset, 16) + F(", restart to start it");
        answer(request, 200, text);
    }

    DEBUG_MSG_P(PSTR("[MODULES] Upload: %s\n"), text.c_str());
    release();
}

void onUploadData(AsyncWebServerRequest* request, String, size_t index, uint8_t* bytes, size_t len, bool) {
    if (!webAuthenticate(request)) {
        return;
    }

    if (!index) {
        if (upload) {
            if (owner) {
                return;
            }
            release();
        }

        if (arch::otaBusy()) {
            return;
        }

        upload = arch::uploadNew(request);
        if (!upload) {
            return;
        }

        upload->replace = request->hasParam(String(F("replace")));
        owner = request;
        request->onDisconnect([request]() {
            if (owner == request) {
                release();
            }
        });
    }

    if (upload && (owner == request)) {
        packageFeed(*upload, bytes, len);
    }
}

// --- apply: the whole module area anew ------------------------------------------------------------------------------
//
// The MODULES panel: modules ticked, *Apply*. First POST /modules/off: the directory erased, a restart without module
// code. Then POST /modules/apply?names=a,b,c with the modules file (EMC2) of the release: the ticked packages are
// written one after another into the empty area (the backend places each under the one before: no gaps, nothing
// fragmented), the directory gets them as they are done, the rest of the file is skipped; then a restart. Nothing runs
// from the area meanwhile: no module code started at this boot, else 409

// the modules file: magic, count, then per package name[16], offset, size, image, RAM, second arena, needs[32], pad
constexpr uint32_t ModulesFileMagic { 0x32434D45 }; // "EMC2"
constexpr size_t ModulesFileEntry { 72 };
constexpr size_t ModulesFileMax { 32 };

// the flags word at the end of an entry of the modules file
constexpr uint32_t ModuleByteRam { 1 };     // its RAM takes byte access from interrupts: never in a 32-bit only pool

struct Apply {
    uint32_t plan_ram[ModulesFileMax] {};   // RAM offset (RamSecondArena: in the fixed pool) per package of the table
    uint32_t plan_iram[ModulesFileMax] {};  // IRAM code offset in the fixed pool (ESP8266)
    char names[DirectoryEntries][16] {};
    size_t names_count { 0 };
    uint8_t head[8] {};
    uint8_t table[ModulesFileMax * ModulesFileEntry] {};
    uint32_t count { 0 };
    size_t received { 0 };
    size_t current { ModulesFileMax };
    const char* error { nullptr };
    String done;
};

Apply* applying { nullptr };

bool applySelected(const char* name) {
    for (size_t index = 0; index < applying->names_count; ++index) {
        if (std::strncmp(applying->names[index], name, sizeof(applying->names[index])) == 0) {
            return true;
        }
    }

    return false;
}

void applyEntry(size_t index, char (&name)[16], uint32_t& offset, uint32_t& size) {
    const uint8_t* entry = &applying->table[index * ModulesFileEntry];
    std::memcpy(name, entry, sizeof(name));
    name[sizeof(name) - 1] = '\0';
    std::memcpy(&offset, entry + 16, 4);
    std::memcpy(&size, entry + 20, 4);
}

uint32_t applyWord(size_t index, size_t at) {
    uint32_t out;
    std::memcpy(&out, &applying->table[(index * ModulesFileEntry) + at], 4);
    return out;
}

// the statics of the selected packages, largest first: the IRAM code into the fixed pool from its start (ESP8266),
// then the RAM into what the fixed pool has left, the rest into the heap block. Every Apply anew: no gaps, nothing kept
const char* applyPlan() {
    auto& state = *applying;
    arch::Pools pools {};
    arch::pools(pools);

    size_t order[ModulesFileMax];
    size_t count = 0;
    for (size_t index = 0; index < state.count; ++index) {
        char name[16];
        uint32_t offset;
        uint32_t size;
        applyEntry(index, name, offset, size);
        if (applySelected(name)) {
            order[count++] = index;
        }
    }

    const auto sorted = [&](size_t at) {
        for (size_t index = 1; index < count; ++index) {
            const size_t value = order[index];
            size_t pos = index;
            while (pos && (applyWord(order[pos - 1], at) < applyWord(value, at))) {
                order[pos] = order[pos - 1];
                --pos;
            }
            order[pos] = value;
        }
    };

    // the flash first: the area is written from empty, every selected module must fit (the directory and its second
    // copy stay out)
    {
        constexpr size_t ImageAt { 24 };
        const auto area = arch::area();
        const uint32_t room = (area.hi > (area.lo + SectorSize)) ? (area.hi - SectorSize - area.lo) : 0;
        uint32_t flash = 0;
        for (size_t index = 0; index < count; ++index) {
            flash += arch::slotFor(applyWord(order[index], ImageAt));
        }
        if (flash > room) {
            DEBUG_MSG_P(PSTR("[MODULES] Apply: the modules need %u bytes of flash, the area has %u\n"), flash, room);
            return PSTR("not enough flash for these modules together");
        }
    }

    constexpr size_t RamAt { 28 };
    constexpr size_t IramAt { 32 };
    constexpr size_t FlagsAt { 68 };

    uint32_t fixed = 0;
    sorted(IramAt);
    for (size_t index = 0; index < count; ++index) {
        const uint32_t iram = roundUp(applyWord(order[index], IramAt), 4);
        if (!iram) {
            continue;
        }
        if (!pools.code_in_fixed || ((fixed + iram) > pools.fixed)) {
            return PSTR("not enough IRAM for the module code");
        }
        state.plan_iram[order[index]] = fixed;
        fixed += iram;
    }

    uint32_t heap = 0;
    sorted(RamAt);
    for (size_t index = 0; index < count; ++index) {
        const uint32_t ram = roundUp(applyWord(order[index], RamAt), 8);
        if (!ram) {
            continue;
        }
        fixed = roundUp(fixed, 8);
        const bool bytes = (applyWord(order[index], FlagsAt) & ModuleByteRam) != 0;
        if (pools.fixed && !(bytes && pools.fixed_words) && ((fixed + ram) <= pools.fixed)) {
            state.plan_ram[order[index]] = fixed | RamSecondArena;
            fixed += ram;
        } else {
            state.plan_ram[order[index]] = heap;
            heap += ram;
        }
    }

    if (heap > pools.heap) {
        DEBUG_MSG_P(PSTR("[MODULES] Apply: the heap block would need %u, the heap gives %u\n"), heap, pools.heap);
        return PSTR("not enough RAM: the heap cannot hold the statics of these modules");
    }

    return nullptr;
}

void applyRelease() {
    release();
    delete applying;
    applying = nullptr;
}

// the package just fed to its end: checked, into the directory
void applyFinish() {
    Entry entry;
    std::memset(&entry, 0, sizeof(entry));
    const auto* error = arch::uploadFinish(*upload, entry);
    if (!error) {
        std::memcpy(entry.name, upload->header.name, sizeof(entry.name));
        entry.name[sizeof(entry.name) - 1] = '\0';
        error = addEntry(entry);
    }

    if (error) {
        applying->error = error;
    } else {
        if (applying->done.length()) {
            applying->done += ' ';
        }
        applying->done += entry.name;
    }

    delete upload;
    upload = nullptr;
    applying->current = ModulesFileMax;
}

void applyData(AsyncWebServerRequest* request, const uint8_t* data, size_t len) {
    auto& state = *applying;
    while (len && !state.error) {
        // the header, then the table of the packages
        if (state.received < sizeof(state.head)) {
            const size_t part = std::min(len, sizeof(state.head) - state.received);
            std::memcpy(&state.head[state.received], data, part);
            state.received += part;
            data += part;
            len -= part;
            if (state.received == sizeof(state.head)) {
                uint32_t magic;
                std::memcpy(&magic, &state.head[0], 4);
                std::memcpy(&state.count, &state.head[4], 4);
                if ((magic != ModulesFileMagic) || !state.count || (state.count > ModulesFileMax)) {
                    state.error = PSTR("not a modules file");
                }
            }
            continue;
        }

        const size_t table = state.count * ModulesFileEntry;
        if (state.received < (sizeof(state.head) + table)) {
            const size_t at = state.received - sizeof(state.head);
            const size_t part = std::min(len, table - at);
            std::memcpy(&state.table[at], data, part);
            state.received += part;
            data += part;
            len -= part;
            // the area anew: the old directory goes, the packages are placed into an empty area
            if ((at + part) == table) {
                state.error = applyPlan();
                if (!state.error && !eraseDirectory()) {
                    state.error = PSTR("directory erase");
                }
            }
            continue;
        }

        // the package this byte belongs to, or the next one
        size_t take = len;
        size_t inside = ModulesFileMax;
        for (size_t index = 0; index < state.count; ++index) {
            char name[16];
            uint32_t offset;
            uint32_t size;
            applyEntry(index, name, offset, size);
            if ((state.received >= offset) && (state.received < (offset + size))) {
                inside = index;
                take = std::min<size_t>(len, (offset + size) - state.received);
                break;
            }
            if (offset > state.received) {
                take = std::min<size_t>(take, offset - state.received);
            }
        }

        if (inside < ModulesFileMax) {
            char name[16];
            uint32_t offset;
            uint32_t size;
            applyEntry(inside, name, offset, size);
            if (applySelected(name)) {
                if (state.current != inside) {
                    upload = arch::uploadNew(request);
                    if (!upload) {
                        state.error = PSTR("no memory for the package");
                        break;
                    }
                    upload->planned = true;
                    upload->planned_ram = state.plan_ram[inside];
                    upload->planned_iram = state.plan_iram[inside];
                    state.current = inside;
                }

                packageFeed(*upload, data, take);
                if (upload->error) {
                    state.error = upload->error;
                    break;
                }

                if ((state.received + take) == (offset + size)) {
                    applyFinish();
                }
            }
        }

        state.received += take;
        data += take;
        len -= take;
    }
}

void onApplyData(AsyncWebServerRequest* request, String, size_t index, uint8_t* bytes, size_t len, bool) {
    if (!webAuthenticate(request)) {
        return;
    }

    if (!index) {
        if (applying || upload || arch::otaBusy()) {
            return;
        }

        applying = new (std::nothrow) Apply;
        if (!applying) {
            return;
        }
        owner = request;
        request->onDisconnect([request]() {
            if (owner == request) {
                applyRelease();
            }
        });

        // names=a,b,c: a form field before the file (a query string is not in the URI the browser's Digest signs and
        // the server checks), or the query
        const auto* param = request->getParam(String(F("names")), true);
        if (!param) {
            param = request->getParam(String(F("names")));
        }
        const String names = param ? param->value() : String();
        int from = 0;
        while ((from < static_cast<int>(names.length())) && (applying->names_count < DirectoryEntries)) {
            int to = names.indexOf(',', from);
            if (to < 0) {
                to = names.length();
            }
            const String name = names.substring(from, to);
            if (name.length()) {
                std::strncpy(applying->names[applying->names_count], name.c_str(),
                    sizeof(applying->names[0]) - 1);
                ++applying->names_count;
            }
            from = to + 1;
        }

        // module code runs from the area: it is rewritten only at a boot that started none
        if (running) {
            applying->error = PSTR("modules run from the area: POST /modules/off first");
        }
    }

    if (applying && (owner == request)) {
        applyData(request, bytes, len);
    }
}

void onApplyDone(AsyncWebServerRequest* request) {
    if (!authenticated(request)) {
        return;
    }

    if (!applying || (owner != request)) {
        answer(request, 409, F("ERROR: no apply in this request (another upload, or modules running)"));
        return;
    }

    const auto* error = applying->error;
    String missing;
    if (!error) {
        for (size_t index = 0; index < applying->names_count; ++index) {
            const String name(applying->names[index]);
            if ((String(' ') + applying->done + ' ').indexOf(String(' ') + name + ' ') < 0) {
                missing += missing.length() ? F(", ") : F("");
                missing += name;
            }
        }
    }

    if (error) {
        const bool conflict = running;
        answer(request, conflict ? 409 : 400, String(F("ERROR: ")) + FPSTR(error));
    } else if (missing.length()) {
        answer(request, 400, String(F("ERROR: not in the modules file or not written: ")) + missing);
    } else {
        answer(request, 200, String(F("OK, applied: ")) + (applying->done.length() ? applying->done : String(F("no modules")))
            + F(", restarting"));
        prepareReset(CustomResetReason::Web);
    }

    DEBUG_MSG_P(PSTR("[MODULES] Apply: %s\n"), error ? error : PSTR("done"));
    applyRelease();
}

// the first step of an apply: no module code at the next boot, the area free to be rewritten
void onModulesOff(AsyncWebServerRequest* request) {
    if (!authenticated(request)) {
        return;
    }

    if (!eraseDirectory()) {
        answer(request, 500, F("ERROR: directory erase"));
        return;
    }

    answer(request, 200, F("OK, modules off, restarting"));
    prepareReset(CustomResetReason::Web);
}

// --- web -----------------------------------------------------------------------------------------------------------

void onStatus(AsyncWebServerRequest* request) {
    if (!authenticated(request)) {
        return;
    }

    Directory dir;
    const bool valid = readDirectory(dir);

    String out;
    out.reserve(1024);
    out += F("{\"version\":\"");
    out += buildApp().version.toString();
    out += F("\",\"image\":\"");
    out += F(ESPURNA_IMAGE);
    out += F("\",\"md5\":\"");
    out += headText();
    out += F("\",\"list\":\"");
    out += getSetting(F("webFwUrl"));
    out += F("\",\"off\":\"");
    out += offReason();
    out += F("\",\"modules\":[");
    for (size_t index = 0; valid && (index < dir.count); ++index) {
        const auto& entry = dir.entries[index];
        if (index) {
            out += ',';
        }
        out += F("{\"name\":\"");
        out += entry.name;
        out += '"';
        jsonField(out, F("size"), entry.size);
        jsonField(out, F("offset"), entry.offset);
        jsonField(out, F("ram"), entry.ram_size);
        jsonField(out, F("heap"), statusOf(entry).heap);
        jsonField(out, F("enabled"), (entry.flags & EntryDisabled) ? 0 : 1);
        jsonField(out, F("placed"), (entry.flags & EntryUnplaced) ? 0 : 1);
        {
            const auto list = requiresOf(entry);
            out += F(",\"requires\":\"");
            for (size_t index = 0; index < list.count; ++index) {
                if (index) {
                    out += ',';
                }
                out += list.names[index];
            }
            out += '"';
        }
        out += (entry.ram_offset & RamSecondArena) ? F(",\"ram_in\":\"iram\"") : F(",\"ram_in\":\"dram\"");
        jsonField(out, F("iram"), arch::entryIram(entry));
        arch::entryJson(out, entry);
        out += F(",\"state\":\"");
        out += stateName(statusOf(entry));
        out += F("\"}");
    }
    out += ']';

    // the meters: Flash from the area's start (the head) to the directory, the arenas
    Spaces space;
    spaces(dir, valid, space);
    const auto area = arch::area();
    jsonField(out, F("area"), area.hi - area.start);
    jsonField(out, F("head"), area.lo - area.start);
    jsonField(out, F("free"), space.flash.free());
    jsonField(out, F("free_max"), space.flash.largest());
    jsonField(out, F("ram_arena"), space.ram.hi);
    jsonField(out, F("ram_used"), space.ram.hi - space.ram.free());
    jsonField(out, F("ram_max"), space.ram.largest());
    {
        // the heap the started modules took when they started (their own allocations, not their statics)
        uint32_t taken = 0;
        for (size_t index = 0; valid && (index < dir.count); ++index) {
            taken += statusOf(dir.entries[index]).heap;
        }
        jsonField(out, F("ram_dynamic"), taken);

        // module IRAM code (ESP8266: in the IRAM tail, with the RAM placed there)
        uint32_t code = 0;
        for (size_t index = 0; valid && (index < dir.count); ++index) {
            if (counted(dir.entries[index], nullptr)) {
                code += arch::entryIram(dir.entries[index]);
            }
        }
        jsonField(out, F("iram_code"), code);
    }
    jsonField(out, F("iram_arena"), space.iram.hi);
    jsonField(out, F("iram_used"), space.iram.hi - space.iram.free());
    jsonField(out, F("iram_max"), space.iram.largest());
    jsonField(out, F("iram_data"), space.ram_second.hi);
    jsonField(out, F("iram_data_used"), space.ram_second.hi - space.ram_second.free());
    jsonField(out, F("iram_data_max"), space.ram_second.largest());

    arch::statusJson(out);
    jsonField(out, F("heap"), arch::freeHeap());
    jsonField(out, F("heap_boot"), systemInitialFreeHeap());
    jsonField(out, F("oom"), arch::failedAllocations());
    jsonField(out, F("file"), arch::fileSize());
    out += '}';

    request->send(200, String(F("application/json")), out);
}

void onFile(AsyncWebServerRequest* request) {
    if (!authenticated(request)) {
        return;
    }

    if (!arch::fileSize()) {
        request->send(404, String(F("text/plain")), String(F("ERROR: no modules file in flash")));
        return;
    }

    arch::fileSend(request);
}

void onModulesOn(AsyncWebServerRequest* request) {
    if (!authenticated(request)) {
        return;
    }

    modulesOffClear();
    request->send(200, String(F("text/plain")), String(F("OK, modules on, restarting")));
    prepareReset(CustomResetReason::Web);
}

// a module out of the directory; its code keeps running until the restart
void onRemove(AsyncWebServerRequest* request) {
    if (!authenticated(request)) {
        return;
    }

    const auto* param = request->getParam(String(F("name")), true);
    if (!param) {
        request->send(400, String(F("text/plain")), String(F("ERROR: no name")));
        return;
    }

    const char* error = nullptr;
    if (!removeEntry(param->value(), error)) {
        request->send(400, String(F("text/plain")), String(F("ERROR: ")) + FPSTR(error));
        return;
    }

    request->send(200, String(F("text/plain")), String(F("OK, removed, restart to apply")));
}

// POST name=, on=1|0
void onEnable(AsyncWebServerRequest* request) {
    if (!authenticated(request)) {
        return;
    }

    const auto* name = request->getParam(String(F("name")), true);
    const auto* on = request->getParam(String(F("on")), true);
    if (!name || !on) {
        request->send(400, String(F("text/plain")), String(F("ERROR: name and on")));
        return;
    }

    String text;
    const auto* error = setEnabled(name->value(), on->value().equals(F("1")), text);
    if (error) {
        request->send(400, String(F("text/plain")), String(F("ERROR: ")) + FPSTR(error));
        return;
    }

    request->send(200, String(F("text/plain")), text);
}

void onRestart(AsyncWebServerRequest* request) {
    if (!authenticated(request)) {
        return;
    }

    request->send(200, String(F("text/plain")), String(F("OK")));
    prepareReset(CustomResetReason::Web);
}

void onPage(AsyncWebServerRequest* request) {
    if (!authenticated(request)) {
        return;
    }

    if (!arch::page(request)) {
        request->send(404, String(F("text/plain")), String(F("no modules page in this image")));
    }
}

// --- terminal ------------------------------------------------------------------------------------------------------

#if TERMINAL_SUPPORT
PROGMEM_STRING(List, "MODULES");
PROGMEM_STRING(Off, "MODULES.OFF");
PROGMEM_STRING(Remove, "MODULES.REMOVE");
PROGMEM_STRING(On, "MODULES.ON");
PROGMEM_STRING(TestLowHeap, "MODULES.TEST.LOWHEAP");
PROGMEM_STRING(TestCrash, "MODULES.TEST.CRASH");
PROGMEM_STRING(Enable, "MODULES.ENABLE");
PROGMEM_STRING(Disable, "MODULES.DISABLE");

void commandSwitch(::terminal::CommandContext&& ctx, bool on) {
    if (ctx.argv.size() != 2) {
        terminalError(ctx, on ? F("MODULES.ENABLE <name>") : F("MODULES.DISABLE <name>"));
        return;
    }

    String text;
    const auto* error = setEnabled(ctx.argv[1], on, text);
    if (error) {
        terminalError(ctx, FPSTR(error));
        return;
    }

    ctx.output.println(text);
    terminalOK(ctx);
}

void commandEnable(::terminal::CommandContext&& ctx) {
    commandSwitch(std::move(ctx), true);
}

void commandDisable(::terminal::CommandContext&& ctx) {
    commandSwitch(std::move(ctx), false);
}

void commandTestLowHeap(::terminal::CommandContext&& ctx) {
    test_low_heap = true;
    terminalOK(ctx);
}

void commandTestCrash(::terminal::CommandContext&& ctx) {
    terminalOK(ctx);
    ctx.output.flush();
    arch::crashNow();
}

void commandOn(::terminal::CommandContext&& ctx) {
    modulesOffClear();
    terminalOK(ctx);
    prepareReset(CustomResetReason::Terminal);
}

void commandList(::terminal::CommandContext&& ctx) {
    Directory dir;
    const bool valid = readDirectory(dir);

    const auto area = arch::area();
    ctx.output.printf_P(PSTR("head %s, area 0x%06x-0x%06x, directory 0x%06x\n"),
        headText().c_str(), area.lo, area.hi, area.hi);
    arch::listHead(ctx.output);

    Spaces space;
    spaces(dir, valid, space);
    ctx.output.printf_P(PSTR("free for modules: flash %u (largest %u), RAM %u (%u)"),
        space.flash.free(), space.flash.largest(), space.ram.free(), space.ram.largest());
    if (space.iram.hi || space.ram_second.hi) {
        ctx.output.printf_P(PSTR(", IRAM %u (%u), RAM in the second arena %u"),
            space.iram.free(), space.iram.largest(), space.ram_second.free());
    }
    ctx.output.println(F(" bytes"));

    if (valid) {
        for (size_t index = 0; index < dir.count; ++index) {
            const auto& entry = dir.entries[index];
            const auto status = statusOf(entry);
            if (entry.flags & EntryUnplaced) {
                ctx.output.printf_P(PSTR("%-15s 0x%05x %6u bytes, RAM %u not placed"),
                    entry.name, entry.offset, entry.size, entry.ram_size);
            } else {
                ctx.output.printf_P(PSTR("%-15s 0x%05x %6u bytes, RAM %u @%u"),
                    entry.name, entry.offset, entry.size, entry.ram_size, entry.ram_offset & ~RamSecondArena);
            }
            arch::listEntry(ctx.output, entry);
            ctx.output.printf_P(PSTR(": %s%s%s\n"), String(stateName(status)).c_str(),
                status.reason ? " - " : "", status.reason ? String(FPSTR(status.reason)).c_str() : "");
        }
    } else {
        ctx.output.println(F("no modules"));
    }

    terminalOK(ctx);
}

void commandOff(::terminal::CommandContext&& ctx) {
    if (!eraseDirectory()) {
        terminalError(ctx, F("erase failed"));
        return;
    }

    terminalOK(ctx);
    prepareReset(CustomResetReason::Terminal);
}

void commandRemove(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() != 2) {
        terminalError(ctx, F("MODULES.REMOVE <name>"));
        return;
    }

    const char* error = nullptr;
    if (!removeEntry(ctx.argv[1], error)) {
        terminalError(ctx, FPSTR(error));
        return;
    }

    terminalOK(ctx);
    prepareReset(CustomResetReason::Terminal);
}

static constexpr ::terminal::Command Commands[] PROGMEM {
    {List, commandList},
    {Off, commandOff},
    {Remove, commandRemove},
    {On, commandOn},
    {TestLowHeap, commandTestLowHeap},
    {TestCrash, commandTestCrash},
    {Enable, commandEnable},
    {Disable, commandDisable},
};
#endif

// the guard watches only while module code runs
void guardArm() {
    if (guard_armed) {
        return;
    }
    guard_armed = true;
    arch::guardState().running = 1;
    espurnaRegisterLoop(guardLoop);
    arch::guardTimer();
}

} // namespace
} // namespace modules
} // namespace espurna

void modulesRamSetup() {
    using namespace espurna::modules;

    // its own copy: readDirectory() reads both copies through `scratch`, the older one would land in it last. Here
    // setup() is shallow, the copy on the stack does not hurt (unlike in the loop, cont_check 2026-11-10)
    Directory dir;
    const uint32_t need = readDirectory(dir) ? heapNeed(dir) : 0;
    // 8 more: the base aligned to 8 bytes whatever the chip's malloc gives (the ESP32 gives 4)
    auto* block = static_cast<uint8_t*>(std::malloc((need ? need : 16) + 8));
    heap_block_base = block ? roundUp(reinterpret_cast<uint32_t>(block), 8) : 0;
    if (need && block) {
        heap_block = block;
        heap_block_size = need;
    } else {
        std::free(block);
    }
}

// before the stability check: the commands are there in the unstable mode as well
void modulesBootSetup() {
    using namespace espurna::modules;

    arch::setup();

#if TERMINAL_SUPPORT
    espurna::terminal::add(Commands);
#endif

    guardBoot();

    Directory dir;
    if (!readDirectory(dir)) {
        return;
    }

    for (size_t index = 0; index < dir.count; ++index) {
        status[index] = Status{State::Skipped, nullptr, dir.entries[index].offset, dir.entries[index].crc32};
    }
}

// stable boot only, where the module's own setup used to be
void modulesSetup() {
    using namespace espurna::modules;

    webServer().on("/modules/upload", HTTP_POST, onUploadDone, onUploadData);
    webServer().on("/modules/apply", HTTP_POST, onApplyDone, onApplyData);
    webServer().on("/modules/off", HTTP_POST, onModulesOff);
    webServer().on("/modules/restart", HTTP_POST, onRestart);
    webServer().on("/modules/remove", HTTP_POST, onRemove);
    webServer().on("/modules/on", HTTP_POST, onModulesOn);
    webServer().on("/modules/enable", HTTP_POST, onEnable);
    espurnaRegisterLoop(pendingLoop);
    webServer().on("/modules/file", HTTP_GET, onFile);
    webServer().on("/modules.html", HTTP_GET, onPage);
    webServer().on("/modules", HTTP_GET, onStatus);

    // the MODULES menu entry of the WebUI module
    wsRegister().onVisible([](JsonObject& root) {
        wsPayloadModule(root, STRING_VIEW("mod"));
    });

    Directory dir;
    bool valid = readDirectory(dir);
    arch::imageModules(dir, valid);
    if (!valid) {
        return;
    }

    if (offReason().length()) {
        for (size_t index = 0; index < dir.count; ++index) {
            status[index] = Status{State::Off, nullptr, dir.entries[index].offset, dir.entries[index].crc32};
        }
        DEBUG_MSG_P(PSTR("[MODULES] Off (%s), MODULES.ON starts them again\n"), offReason().c_str());
        return;
    }

    arch::beforeStart(dir, valid);

    // switched off: their RAM back first (nothing runs yet), the ones placed below may take it. Beken 10.0.5.139
    // 2026-11-09: mqtt switched off, restart, matter switched on still found its RAM taken by mqtt
    bool given = false;
    for (size_t index = 0; index < dir.count; ++index) {
        auto& entry = dir.entries[index];
        if (entry.flags & EntryDisabled) {
            given = giveBack(entry) || given;
        }
    }
    if (given && !writeDirectory(dir)) {
        DEBUG_MSG_P(PSTR("[MODULES] Directory write failed\n"));
    }

    // switched on, RAM not placed (the modules on by default of the full image, the ones the head's image brings): placed
    // first. Their slot holds the image only then (the ones of the head's tail: nothing yet), and the order below reads
    // its header (priority, what it needs). One that cannot be placed stays out (Broken, why), the start skips it
    bool unplaced[DirectoryEntries] {};
    for (size_t index = 0; index < dir.count; ++index) {
        auto& entry = dir.entries[index];
        if ((entry.flags & EntryDisabled) || !(entry.flags & EntryUnplaced)) {
            continue;
        }
        const auto* error = placeNow(dir, entry);
        if (error) {
            unplaced[index] = true;
            status[index] = Status{State::Broken, error, entry.offset, entry.crc32};
            std::memcpy(status[index].name, entry.name, sizeof(status[index].name));
            DEBUG_MSG_P(PSTR("[MODULES] %s not placed: %s\n"), entry.name, error);
        }
    }

    // providers of an API (the sensors) before their users, by the header's priority; a module after the modules it
    // needs (passes: each starts the modules whose needs are running)
    size_t order[DirectoryEntries];
    uint32_t priority[DirectoryEntries];
    for (size_t index = 0; index < dir.count; ++index) {
        Header header;
        priority[index] = readHeader(dir.entries[index], header) ? header.priority : 0xffffffff;
        size_t pos = index;
        while (pos && (priority[order[pos - 1]] > priority[index])) {
            order[pos] = order[pos - 1];
            --pos;
        }
        order[pos] = index;
    }

    bool done[DirectoryEntries] {};
    bool changed = false;
    for (size_t index = 0; index < dir.count; ++index) {
        auto& entry = dir.entries[index];
        if (entry.flags & EntryDisabled) {
            // off: its RAM back to the arenas (still placed when it was switched off while it ran)
            changed = giveBack(entry) || changed;
            status[index] = Status{State::Disabled, nullptr, entry.offset, entry.crc32};
            std::memcpy(status[index].name, entry.name, sizeof(status[index].name));
            done[index] = true;
        } else if (unplaced[index]) {
            // not placed above: Broken with its reason already
            done[index] = true;
        } else {
            status[index] = Status{};
        }
    }

    for (size_t pass = 0; pass <= dir.count; ++pass) {
        bool started_any = false;
        for (size_t position = 0; position < dir.count; ++position) {
            const size_t index = order[position];
            if (done[index]) {
                continue;
            }
            const auto& entry = dir.entries[index];
            const auto list = requiresOf(entry);
            bool ready = true;
            for (size_t need = 0; need < list.count; ++need) {
                ready = ready && runningByName(list.names[need]);
            }
            if (!ready) {
                continue;
            }
            done[index] = true;
            started_any = true;
            auto& target = dir.entries[index];
            if (changed) {
                writeDirectory(dir);
                changed = false;
            }
            const auto* error = placeNow(dir, target);
            if (error) {
                status[index] = Status{State::Broken, error, target.offset, target.crc32};
                std::memcpy(status[index].name, target.name, sizeof(status[index].name));
                DEBUG_MSG_P(PSTR("[MODULES] %s not started: %s\n"), target.name, error);
                continue;
            }
            startEntry(target);
        }
        if (!started_any) {
            break;
        }
    }

    // RAM given back by the modules that are off
    if (changed) {
        writeDirectory(dir);
    }

    // what is left needs a module that is not installed, off, or did not start
    for (size_t index = 0; index < dir.count; ++index) {
        if (!done[index]) {
            const auto& entry = dir.entries[index];
            status[index] = Status{State::Broken, PSTR("needs a module that is not running"), entry.offset, entry.crc32};
            std::memcpy(status[index].name, entry.name, sizeof(status[index].name));
            DEBUG_MSG_P(PSTR("[MODULES] %s not started: needs a module that is not running\n"), entry.name);
        }
    }
}

bool modulesRunning() {
    return espurna::modules::running;
}

bool modulesEnabled(const char* name) {
    using namespace espurna::modules;
    std::unique_ptr<Directory> dir(new (std::nothrow) Directory);
    if (!dir || !readDirectory(*dir)) {
        return false;
    }

    for (size_t index = 0; index < dir->count; ++index) {
        const auto& entry = dir->entries[index];
        if ((std::strncmp(entry.name, name, sizeof(entry.name)) == 0) && !(entry.flags & EntryDisabled)) {
            return true;
        }
    }

    return false;
}

void modulesOnHome(AsyncWebServerRequest* request) {
    using namespace espurna::modules;
    if (!authenticated(request)) {
        return;
    }

    for (size_t index = 0; index < data_count; ++index) {
        if (std::strcmp(data[index].name, "webui") == 0) {
            auto* response = request->beginResponse_P(200, String(F("text/html")), data[index].data, data[index].size);
            response->addHeader(String(F("Content-Encoding")), String(F("gzip")));
            request->send(response);
            return;
        }
    }

    if (!arch::page(request)) {
        request->send(404, String(F("text/plain")), String(F("no WebUI module")));
    }
}

void modulesForget() {
    espurna::modules::eraseDirectory();
}

#endif // MODULES_SUPPORT && MODULES_CORE
