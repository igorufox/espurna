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

#include "espurna_ota.h"
#include "espurna_tasks.h"
#include "modules_core.h"
#include "modules_page.h"
#include "modules_package.h"
#include "libs/crc32_ieee.h"
#include "libs/text.h"
#include "terminal.h"
#include "web.h"
#include "ws.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>

#if defined(ESP8266)
#include <osapi.h>
#elif __has_include(<freertos/FreeRTOS.h>)
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#else
#include <FreeRTOS.h>
#include <timers.h>
#endif

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

// long enough for what the modules start late: Matter starts 75 s after the Wi-Fi on the Beken and its hang came
// ~25 s later, every boot, with 60 here nothing stopped that loop (10.0.5.139, 11-11)
#ifndef MODULES_BOOT_SECONDS
#define MODULES_BOOT_SECONDS 300
#endif

namespace espurna {
namespace modules {

// --- small things --------------------------------------------------------------------------------------------------

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t size) {
    return crc32_ieee::crc32Update(crc, data, size);
}

uint32_t roundUp(uint32_t value, uint32_t to) {
    return package::roundUp(value, to);
}

bool insideRange(uint32_t start, uint32_t end, uint32_t lo, uint32_t hi) {
    return (start <= end) && (start >= lo) && (end <= hi);
}

// a JSON string value, escaped: a module name or a reason with a quote does not break the panel (libs/text.h)
using espurna::text::jsonString;

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

} // namespace modules
} // namespace espurna

// The identity of the head, every chip alike: the GNU build id of its link, kept in its flash by the linker
// (scripts/module_head.py): the note header (16 bytes), then the id. Modules carry its first 16 bytes (head_md5 of
// the header, the name stayed). Nothing is hashed at run time (the MD5 of the image used to be read from the flash)
extern "C" const uint8_t __build_id_start[];

namespace espurna {
namespace modules {
namespace {

bool headBytes(uint8_t out[16]) {
    memcpy_P(out, __build_id_start + 16, 16);
    return true;
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
    return (dir.magic == DirectoryMagic) &&(dir.count <= DirectoryEntries) && directoryNames(dir);
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

// the directory and `scratch` are read and written from the loop, the web callbacks and the guard's timer (plan 3.5)
espurna::system::Mutex directory_mutex;

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
        if (entry.ram_size && !(entry.ram_offset & RamSecondArena)) {
            need = std::max<uint32_t>(need, entry.ram_offset + entry.ram_size);
        }
    }

    return roundUp(need, 16);
}

bool readDirectory(Directory& out) {
    espurna::system::MutexLock lock(directory_mutex);
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
    espurna::system::MutexLock lock(directory_mutex);
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

    // an empty directory first, one write with a newer generation: a power cut between the two erases below would
    // otherwise leave the older copy, and the modules came back after an "off" (review 1.10)
    auto empty = boxed<Directory>();
    if (empty) {
        empty->magic = DirectoryMagic;
        empty->count = 0;
        writeDirectory(*empty);
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
    auto dir_box = boxed<Directory>();
    if (!dir_box) {
        return String();
    }
    auto& dir = *dir_box;
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

// the WebUI is the webui data module on every chip (decision 11-11); the head keeps its own small page


void offWrite(const char* reason) {
    auto dir_box = boxed<Directory>();
    if (!dir_box) {
        return;
    }
    auto& dir = *dir_box;
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
    auto dir_box = boxed<Directory>();
    if (!dir_box) {
        return;
    }
    auto& dir = *dir_box;
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

// where a module runs in this boot, from its bases: the image as data (header, .data image, constructors), its code
// (the data view, ESP32 the instruction view, ESP8266 the IRAM code in the fixed pool), its statics
struct Runs {
    uint32_t flash_lo;
    uint32_t flash_hi;
    uint32_t entry_lo;
    uint32_t entry_hi;
    uint32_t ram_lo;
    uint32_t ram_hi;
    uint32_t iram_lo;
    uint32_t iram_hi;
    bool iram_copy;     // the IRAM code copied from iram_load into the fixed pool
    bool words;         // 32-bit loads and stores only
};

// the address of the statics of an entry with this offset, in this boot
uint32_t ramAddress(uint32_t ram_offset, const arch::Pools& pools) {
    return (ram_offset & RamSecondArena)
        ? (pools.fixed_base + (ram_offset & ~RamSecondArena))
        : (heapBlockBase() + ram_offset);
}

// the bases a module needs in this boot
void basesNow(const Entry& entry, const arch::Pools& pools, uint32_t (&out)[3]) {
    out[BaseFlash] = arch::flashView(entry.offset);
    out[BaseCode] = entry.code_size ? entry.bases[BaseCode] : arch::codeView(entry.offset);
    out[BaseRam] = entry.ram_size ? ramAddress(entry.ram_offset, pools) : entry.bases[BaseRam];
}

const char* runsOf(const Entry& entry, const Header& header, Runs& out) {
    arch::Pools pools {};
    arch::pools(pools);

    uint32_t now[3];
    basesNow(entry, pools, now);
    if (std::memcmp(now, entry.bases, sizeof(now)) != 0) {
        return PSTR("not relocated for this boot: Apply again");
    }

    if (entry.ram_size && !(entry.ram_offset & RamSecondArena)
        && ((entry.ram_offset + entry.ram_size) > heapBlockSize()))
    {
        return PSTR("not in this boot's heap block: Apply again");
    }

    const uint32_t code = arch::codeView(entry.offset);
    out.flash_lo = entry.bases[BaseFlash];
    out.flash_hi = out.flash_lo + entry.size;
    out.entry_lo = code ? code : out.flash_lo;
    out.entry_hi = out.entry_lo + entry.size;
    out.ram_lo = entry.bases[BaseRam];
    out.ram_hi = out.ram_lo + entry.ram_size;
    if (entry.code_size) {
        out.iram_lo = entry.bases[BaseCode];
        out.iram_hi = out.iram_lo + entry.code_size;
        out.iram_copy = true;
    } else {
        out.iram_lo = code;
        out.iram_hi = code ? (code + entry.size) : 0;
        out.iram_copy = false;
    }
    out.words = pools.words;
    (void)header;
    return nullptr;
}

// the image checked, its ranges against where it runs; its data copied, its bss cleared, its IRAM code copied, its
// constructors and its entry run. nullptr - started; PSTR("stale") - made for another head; else why not. A data
// module (header.entry 0) gives its bytes
void guardArm();

const char* run(const Entry& entry, Started& out) {
    Header header;
    const auto* error = checkImage(entry, header);
    if (error) {
        return error;
    }

    Runs runs {};
    error = runsOf(entry, header, runs);
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
    auto dir_box = boxed<Directory>();
    if (!dir_box) {
        error = PSTR("no memory"); return false;
    }
    auto& dir = *dir_box;
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

} // namespace

// --- placing -------------------------------------------------------------------------------------------------------

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

// the flash between the head and the directory's second copy, the heap block as big as the heap may give it, the fixed
// pool; with what the modules of the directory take
void spaces(const Directory& dir, bool valid, Spaces& out) {
    const auto area = arch::area();
    out.flash.lo = area.lo;
    out.flash.hi = (area.hi >= (area.lo + SectorSize)) ? (area.hi - SectorSize) : area.lo;

    arch::Pools pools {};
    arch::pools(pools);
    out.ram.lo = 0;
    out.ram.hi = pools.heap;
    out.fixed.lo = 0;
    out.fixed.hi = pools.fixed;

    for (size_t index = 0; valid && (index < dir.count); ++index) {
        const auto& entry = dir.entries[index];
        out.flash.add(entry.offset, entry.slot);
        if (entry.ram_offset & RamSecondArena) {
            out.fixed.add(entry.ram_offset & ~RamSecondArena, entry.ram_size);
        } else {
            out.ram.add(entry.ram_offset, entry.ram_size);
        }
        if (entry.code_size) {
            out.fixed.add(entry.bases[BaseCode] - pools.fixed_base, entry.code_size);
        }
    }
}

// --- the guard's count: boot marks in the directory sector ---------------------------------------------------------
//
// The same on every chip (Beken clears the whole RAM at every reset: nothing survives a crash there): a pair of words
// per boot that started module code, written without an erase (bits only go 1 -> 0): [0] cleared when the code
// starts, [1] once the boot ran MODULES_BOOT_SECONDS or ended by a restart of ESPurna. A directory write erases them

namespace {

constexpr uint32_t MarksOffset { 2048 };
constexpr size_t Marks { 64 };

struct GuardState {
    uint32_t running;   // the boot before this one started module code
    uint32_t booted;    // ... and it ran past MODULES_BOOT_SECONDS
    uint32_t crashes;   // boots in a row before it that started module code and did not get there
};

GuardState guard_state {};
size_t boot_mark { Marks };
bool boot_mark_finished { false };

bool readMarks(uint32_t (&marks)[Marks * 2]) {
    const auto area = arch::area();
    return area.hi && arch::rawRead(area.hi + MarksOffset, marks, sizeof(marks));
}

bool clearMark(size_t word) {
    const auto area = arch::area();
    const uint32_t zero { 0 };
    return area.hi && arch::rawWrite(area.hi + MarksOffset + (word * 4), &zero, sizeof(zero));
}

// boots in a row that started module code and did not get to their end mark
size_t unfinishedBoots(const uint32_t (&marks)[Marks * 2]) {
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

// the boot before this one ended by a crash (an exception, a watchdog), the chip's reset reason
bool crashRestart() {
    switch (espurna::system::chip::reset_reason()) {
    case REASON_EXCEPTION_RST:
    case REASON_WDT_RST:
    case REASON_SOFT_WDT_RST:
        return true;
    }

    return false;
}

void guardLoad() {
    uint32_t marks[Marks * 2];
    if (!readMarks(marks)) {
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
    guard_state.running = (last < Marks) ? 1 : 0;
    guard_state.booted = ((last < Marks) && (marks[last * 2 + 1] == 0)) ? 1 : 0;
    // guardBoot() adds the last boot when it crashed
    guard_state.crashes = (guard_state.running && !guard_state.booted && unfinished) ? (unfinished - 1) : unfinished;
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
        auto dir = boxed<Directory>();
        if (!dir || !readDirectory(*dir) || !writeDirectoryPrimary(*dir)) {
            return;
        }
        index = 0;
    }

    if (clearMark(index * 2)) {
        boot_mark = index;
    }
}

// this boot ran MODULES_BOOT_SECONDS with module code
void guardBooted() {
    if (!boot_mark_finished && (boot_mark < Marks)) {
        boot_mark_finished = true;
        clearMark(boot_mark * 2 + 1);
    }
}

// a timer that runs while loop() waits or hangs: the SDK's on the ESP8266, a FreeRTOS one (its own task) elsewhere
#if defined(ESP8266)
os_timer_t guard_timer;

void guardTimerCallback(void*) {
    guardTick();
}

void guardTimer() {
    os_timer_setfn(&guard_timer, guardTimerCallback, nullptr);
    os_timer_arm(&guard_timer, 1000, true);
}

void guardTimerStop() {
    os_timer_disarm(&guard_timer);
}
#else
TimerHandle_t guard_timer { nullptr };

void guardTimerCallback(TimerHandle_t) {
    guardTick();
}

void guardTimer() {
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
#endif

} // namespace

// --- a package as it arrives ---------------------------------------------------------------------------------------

namespace {

// Upload::part
constexpr uint8_t PartMap { 0 };
constexpr uint8_t PartCount { 1 };
constexpr uint8_t PartBranches { 2 };
constexpr uint8_t PartChunk { 3 };

// the header alone: the package magic and the version, made for this head, the sizes
const char* packageCheck(const PackageHeader& header) {
    if ((header.magic != PackageMagic) || (header.version != PackageVersion)) {
        return PSTR("not a module package of this version");
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

uint32_t mapsSize(uint32_t image) {
    return ((image + ChunkSize - 1) / ChunkSize) * MapSize;
}

// the slot: the image in the chip's flash format, then its relocation maps
uint32_t slotSize(uint32_t image) {
    return arch::imageFlash(image) + roundUp(mapsSize(image), SectorSize);
}

uint32_t mapsAt(uint32_t slot, uint32_t image) {
    return slot + arch::imageFlash(image);
}

// the header is in: its slot from the top of what the Apply left free, its bases, the deltas of its kinds
const char* prepare(Upload& up) {
    const auto& header = up.header;
    const auto* error = packageCheck(header);
    if (error) {
        return error;
    }

    const auto& dir = *up.apply_dir;
    for (size_t index = 0; index < dir.count; ++index) {
        if (std::strncmp(dir.entries[index].name, header.name, sizeof(dir.entries[index].name)) == 0) {
            return PSTR("twice in the Apply");
        }
    }
    if (dir.count >= DirectoryEntries) {
        return PSTR("directory full");
    }

    auto space = boxed<Spaces>();
    if (!space) {
        return PSTR("no memory");
    }
    spaces(dir, true, *space);
    space->flash.lo = std::max(space->flash.lo, up.flash_floor);

    up.slot_size = slotSize(header.image_size);
    if (!space->flash.place(up.slot_size, true, up.slot)) {
        return PSTR("not enough flash");
    }

    arch::Pools pools {};
    arch::pools(pools);
    if (header.iram_size && !pools.code_in_fixed) {
        return PSTR("IRAM code, and the chip has no pool for it");
    }

    up.bases[BaseFlash] = arch::flashView(up.slot);
    up.bases[BaseCode] = header.iram_size ? (pools.fixed_base + up.code_offset) : arch::codeView(up.slot);
    up.bases[BaseRam] = header.ram_size ? ramAddress(up.ram_offset, pools) : 0;
    up.delta[0] = 0;
    up.delta[1] = up.bases[BaseFlash] - header.link_flash;
    up.delta[2] = up.bases[BaseRam] - header.link_ram;
    up.delta[3] = up.bases[BaseCode] - header.link_iram;

    // the maps behind the image: erased now, written chunk by chunk
    if (!flashErase(mapsAt(up.slot, header.image_size), roundUp(mapsSize(header.image_size), SectorSize))) {
        return PSTR("flash erase");
    }

    return arch::imageBegin(up);
}

// a whole chunk is in: its map kept behind the image, its words relocated, written
const char* chunkDone(Upload& up) {
    const uint32_t offset = up.written;
    const uint32_t maps = mapsAt(up.slot, up.header.image_size);
    if (!flashWrite(maps + ((offset / ChunkSize) * MapSize), up.map, MapSize)) {
        return PSTR("flash write");
    }

    relocateChunk(up.chunk, up.chunk_fill, up.map, up.delta);
    const bool last = (offset + up.chunk_fill) == up.header.image_size;
    const auto* error = arch::imageWrite(up, offset, up.chunk, up.chunk_fill, last);
    up.written += up.chunk_fill;
    up.chunk_fill = 0;
    return error;
}

} // namespace

void relocateChunk(uint8_t* data, size_t size, const uint8_t* map, const uint32_t delta[4]) {
    for (size_t index = 0; index < (size / 4); ++index) {
        const uint8_t kind = (map[index >> 2] >> ((index & 3) * 2)) & 3;
        if (kind && delta[kind]) {
            uint32_t value;
            std::memcpy(&value, &data[index * 4], 4);
            value += delta[kind];
            std::memcpy(&data[index * 4], &value, 4);
        }
    }
}

namespace {

void packageFeed(Upload& up, const uint8_t* data, size_t len) {
    while (len && !up.error) {
        // the package header
        if (up.received < sizeof(PackageHeader)) {
            const size_t part = std::min(len, sizeof(PackageHeader) - up.received);
            std::memcpy(reinterpret_cast<uint8_t*>(&up.header) + up.received, data, part);
            up.received += part;
            data += part;
            len -= part;
            if (up.received == sizeof(PackageHeader)) {
                up.error = prepare(up);
            }
            continue;
        }

        if (up.delivered >= up.header.image_size) {
            up.error = PSTR("more data than the image");
            return;
        }

        // the chunk's bytes
        if (up.part == PartChunk) {
            const size_t part = std::min(len, up.chunk_left);
            up.body_crc = crc32Update(up.body_crc, data, part);
            std::memcpy(&up.chunk[up.chunk_fill], data, part);
            up.chunk_fill += part;
            up.delivered += part;
            up.chunk_left -= part;
            up.received += part;
            data += part;
            len -= part;
            if (!up.chunk_left) {
                up.part = PartMap;
                up.error = chunkDone(up);
            }
            continue;
        }

        // the map of the next chunk, its branch count, its branch list
        uint8_t* target { up.map };
        size_t want { MapSize };
        if (up.part == PartCount) {
            target = reinterpret_cast<uint8_t*>(&up.branch_count);
            want = sizeof(up.branch_count);
        } else if (up.part == PartBranches) {
            target = reinterpret_cast<uint8_t*>(up.branches.get());
            want = up.branch_count * 4;
        }

        const size_t part = std::min(len, want - up.fill);
        std::memcpy(target + up.fill, data, part);
        up.body_crc = crc32Update(up.body_crc, data, part);
        up.fill += part;
        up.received += part;
        data += part;
        len -= part;
        if (up.fill < want) {
            continue;
        }

        up.fill = 0;
        if (up.part == PartMap) {
            up.part = PartCount;
            continue;
        }

        if (up.part == PartCount) {
            if (up.branch_count > BranchesMax) {
                up.error = PSTR("too many branches in a chunk");
                return;
            }
            if (up.branch_count) {
                if (!up.branches) {
                    up.branches.reset(new (std::nothrow) uint32_t[BranchesMax]);
                }
                if (!up.branches) {
                    up.error = PSTR("no memory for the branch list");
                    return;
                }
                up.part = PartBranches;
                continue;
            }
        }

        up.chunk_left = std::min<size_t>(ChunkSize, up.header.image_size - up.delivered);
        up.part = PartChunk;
    }
}

// the end of the package: the whole package came, its CRC, the image read back as it runs
const char* packageFinish(Upload& up, Entry& out) {
    if (up.error) {
        return up.error;
    }

    if ((up.received < sizeof(PackageHeader)) || (up.delivered != up.header.image_size)
        || (up.written != up.header.image_size))
    {
        return PSTR("short package");
    }

    if (up.body_crc != up.header.crc32) {
        return PSTR("package crc");
    }

    std::memset(&out, 0, sizeof(out));
    out.offset = up.slot;
    out.slot = up.slot_size;
    out.size = up.header.image_size;
    out.ram_offset = up.ram_offset;
    out.ram_size = roundUp(up.header.ram_size, 8);
    out.code_size = roundUp(up.header.iram_size, 4);
    std::memcpy(out.bases, up.bases, sizeof(out.bases));

    arch::imageDone(up.slot, up.header.image_size);
    const auto at = arch::imageAt(out);
    uint32_t crc;
    if (!at || !imageCrc(at, out.size, crc)) {
        return PSTR("verify: read");
    }
    if (crc != up.image_crc) {
        return PSTR("verify: the image in flash");
    }

    out.crc32 = crc;
    return nullptr;
}

// --- relocation at boot: the bases of a module moved since the Apply (ESP32: the maps of the flash came out
// elsewhere, every chip: the heap block). The image rewritten in its slot from its relocation maps, chunk by chunk

const char* relocateEntry(Entry& entry, const uint32_t (&now)[3]) {
    if (!arch::rewritable()) {
        return PSTR("moved since the Apply: Apply again");
    }

    auto up = boxed<Upload>();
    if (!up) {
        return PSTR("no memory to relocate");
    }

    const uint32_t delta[4] {0, now[BaseFlash] - entry.bases[BaseFlash], now[BaseRam] - entry.bases[BaseRam],
        now[BaseCode] - entry.bases[BaseCode]};
    const uint32_t at = arch::imageAt(entry);
    const uint32_t maps = mapsAt(entry.offset, entry.size);
    up->slot = entry.offset;
    up->slot_size = entry.slot;
    up->header.image_size = entry.size;
    up->image_crc = 0;
    for (uint32_t done = 0; done < entry.size; done += ChunkSize) {
        const uint32_t size = std::min<uint32_t>(ChunkSize, entry.size - done);
        if (!arch::imageRead(at + done, up->chunk, size)
            || !arch::rawRead(maps + ((done / ChunkSize) * MapSize), up->map, MapSize))
        {
            return PSTR("relocate: read");
        }

        relocateChunk(up->chunk, size, up->map, delta);
        up->branch_count = 0;
        const auto* error = arch::imageWrite(*up, done, up->chunk, size, (done + size) == entry.size);
        if (error) {
            return error;
        }
    }

    arch::imageDone(entry.offset, entry.size);
    entry.crc32 = up->image_crc;
    std::memcpy(entry.bases, now, sizeof(entry.bases));
    return nullptr;
}

} // namespace

// --- the defaults of what a chip provides --------------------------------------------------------------------------

namespace arch {

// plain flash: the image is read where it is stored
__attribute__((weak)) uint32_t imageAt(const Entry& entry) {
    return entry.offset;
}

__attribute__((weak)) bool imageRead(uint32_t at, void* data, size_t size) {
    return rawRead(at, data, size);
}

__attribute__((weak)) uint32_t imageFlash(uint32_t image) {
    return roundUp(image, SectorSize);
}

// plain flash: each sector erased as the chunk reaches it, the words written as they are
__attribute__((weak)) const char* imageWrite(Upload& up, uint32_t offset, uint8_t* data, size_t size, bool) {
    const uint32_t address = up.slot + offset;
    if (!flashErase(address, roundUp(size, SectorSize)) || !flashWrite(address, data, roundUp(size, 4))) {
        return PSTR("flash write");
    }

    up.image_crc = crc32Update(up.image_crc, data, size);
    return nullptr;
}

__attribute__((weak)) const char* imageBegin(Upload&) {
    return nullptr;
}

__attribute__((weak)) void imageDone(uint32_t, uint32_t) {
}

__attribute__((weak)) bool rewritable() {
    return true;
}

__attribute__((weak)) void crashNow() {
    *reinterpret_cast<volatile uint32_t*>(0) = 0;
}

__attribute__((weak)) void statusJson(String&) {
}

__attribute__((weak)) void listHead(Print&) {
}

__attribute__((weak)) uint32_t failedAllocations() {
    return 0;
}

__attribute__((weak)) bool otaBusy() {
    return otaWebUploading();
}

} // namespace arch

namespace {

// the head's own page (modules_page.h): the same on every chip
// a page from the flash (PROGMEM, the module area), a piece at a time
void sendFlash(WebRequest& request, const uint8_t* data, size_t size, bool gzip) {
    if (gzip) {
        request.addHeader("Content-Encoding", "gzip");
    }

    request.send(200, "text/html", static_cast<long>(size),
        [data, size](uint8_t* out, size_t max, size_t index) -> size_t {
            const size_t take = (index < size) ? std::min(max, size - index) : 0;
            if (take) {
                memcpy_P(out, data + index, take);
            }
            return take;
        });
}

void headPage(WebRequest& request) {
    sendFlash(request, reinterpret_cast<const uint8_t*>(HeadPage), sizeof(HeadPage) - 1, false);
}

} // namespace

namespace {

// --- the guard: modules off after crashes at boot, a heap shortage, failing allocations or a stalled loop --------

// MODULES.TEST.LOWHEAP: the guard sees no free heap
bool test_low_heap { false };

void guardBoot() {
    guardLoad();
    auto& guard = guard_state;

    // a boot crash: the modules ran, but not past their first MODULES_BOOT_SECONDS. Boots without module code (the
    // unstable mode after the crashes, its exit restart) keep the count, so the crash loop ends here and not in
    // ESPurna's unstable mode every 10 minutes; modules running past their boot clear it
    if (crashRestart() && guard.running && !guard.booted) {
        ++guard.crashes;
    } else if (guard.running && guard.booted) {
        guard.crashes = 0;
    }

    if (guard.crashes >= MODULES_CRASHES) {
        modulesOff(String(F("the boot crashed ")) + String(guard.crashes, 10)
            + F(" times in a row with the modules running"));
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
    guardTimerStop();
    offWrite(reason);
    customResetReason(CustomResetReason::Stability);
    espurna::system::chip::restart();
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

    if (seconds >= MODULES_BOOT_SECONDS) {
        guardBooted();
    }

    char reason[96];

    const auto free = test_low_heap ? 0 : systemFreeHeap();
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

bool runningByName(const char* name) {
    for (const auto& out : status) {
        if ((out.state == State::Started) && (std::strncmp(out.name, name, sizeof(out.name)) == 0)) {
            return true;
        }
    }
    return false;
}

void setStatus(const Entry& entry, State state, const char* reason) {
    if (auto* slot = statusSlot(entry)) {
        *slot = Status{state, reason, entry.offset, entry.crc32};
        std::memcpy(slot->name, entry.name, sizeof(slot->name));
    }
}

// one module started, at boot
const char* startEntry(const Entry& entry) {
    auto* slot = statusSlot(entry);
    if (!slot) {
        return PSTR("no status slot");
    }

    const uint32_t heap_before = systemFreeHeap();
    Started started;
    // run() arms the guard right before the module's first code (its constructors, entry())
    const auto* error = run(entry, started);
    if (error) {
        const bool stale = (strcmp_P("stale", error) == 0);
        setStatus(entry, stale ? State::Stale : State::Broken, error);
        DEBUG_MSG_P(PSTR("[MODULES] %s not started: %s\n"), entry.name, error);
        return error;
    }

    const uint32_t heap_after = systemFreeHeap();
    *slot = Status{State::Started, nullptr, entry.offset, entry.crc32,
        (heap_before > heap_after) ? (heap_before - heap_after) : 0};
    std::memcpy(slot->name, entry.name, sizeof(slot->name));
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

// switched on with what it needs (installed ones only), the deepest first
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
    return nullptr;
}

// on: the flag off for it and what it needs; off: refused while a switched-on module needs it. Both at the next boot:
// nothing starts or stops while the board runs (the Apply's rule)
const char* setEnabled(const String& name, bool on, String& text) {
    auto dir_box = boxed<Directory>();
    if (!dir_box) {
        return PSTR("no memory");
    }
    auto& dir = *dir_box;
    if (!readDirectory(dir)) {
        return PSTR("no modules");
    }

    const auto* entry = findEntry(dir, name.c_str());
    if (!entry) {
        return PSTR("not installed");
    }

    bool changed = false;
    if (on) {
        const auto* error = enableTree(dir, entry->name, 0, changed);
        if (error) {
            return error;
        }
    } else {
        if (neededBy(dir, entry->name, true)) {
            return PSTR("a switched on module needs it, switch that off first");
        }
        if (!(entry->flags & EntryDisabled)) {
            const_cast<Entry*>(entry)->flags |= EntryDisabled;
            changed = true;
        }
    }

    if (changed && !writeDirectory(dir)) {
        return PSTR("directory write");
    }
    text = on ? F("OK, on after the restart") : F("OK, off after the restart");
    return nullptr;
}

// --- web helpers ---------------------------------------------------------------------------------------------------

WebRequest* owner { nullptr };

void answer(WebRequest& request, int code, const String& text) {
    webSend(request, code, "text/plain", text);
}

bool authenticated(WebRequest& request) {
    if (!webAuthenticate(request)) {
        webRequestAuthentication(request);
        return false;
    }

    return true;
}


// --- apply: the whole module area anew ------------------------------------------------------------------------------
//
// The MODULES panel: modules ticked, *Apply*. First POST /modules/off: the off mark, a restart without module code.
// Then POST /modules/apply?names=a,b,c with the modules file (EMC2) of the release: the ticked packages are written one
// after another into the area from the top down (no gaps, nothing fragmented), the new directory is built in memory
// and written once at the end, the rest of the file is skipped; then a restart. Nothing runs from the area meanwhile:
// no module code started at this boot, else 409.
//
// The initial image of every chip carries a modules file of the modules on out of the box at the start of the area
// (the captive portal): a boot with no directory of this head installs them from there by the same Apply

// the modules file: magic, count, then per package name[16], offset, size, image, RAM, IRAM, needs[32], flags
constexpr uint32_t ModulesFileMagic { 0x32434D45 }; // "EMC2"
constexpr size_t ModulesFileEntry { 72 };
constexpr size_t ModulesFileMax { 32 };

// the flags word at the end of an entry of the modules file
constexpr uint32_t ModuleByteRam { 1 };     // its statics take byte access from interrupts: never in a 32-bit only pool

struct Apply {
    // the new directory, built here as the packages are written and put into flash once, at the end: an Apply that
    // breaks off leaves the old one (with the off mark of /modules/off), never an empty area
    Directory fresh {};
    uint32_t plan_ram[ModulesFileMax] {};   // RAM offset (RamSecondArena: in the fixed pool) per package of the table
    uint32_t plan_code[ModulesFileMax] {};  // IRAM code offset in the fixed pool (ESP8266)
    char names[DirectoryEntries][16] {};
    size_t names_count { 0 };
    bool every { false };                   // every package of the file (the initial image's)
    uint32_t floor { 0 };                   // no slot under it (the initial image's modules file lies there)
    uint8_t head[8] {};
    uint8_t table[ModulesFileMax * ModulesFileEntry] {};
    uint32_t count { 0 };
    size_t received { 0 };
    size_t current { ModulesFileMax };
    std::unique_ptr<Upload> upload;
    const char* error { nullptr };
    String done;
};

Apply* applying { nullptr };

bool applyRunning() {
    return applying != nullptr;
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

bool applySelected(const char* name) {
    if (applying->every) {
        return true;
    }

    for (size_t index = 0; index < applying->names_count; ++index) {
        if (std::strncmp(applying->names[index], name, sizeof(applying->names[index])) == 0) {
            return true;
        }
    }

    return false;
}

// the statics of the selected packages, largest first: the IRAM code into the fixed pool from its start (ESP8266),
// then the statics into what the fixed pool has left, the rest into the heap block. Every Apply anew: no gaps
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
    // copy stay out, and the initial image's modules file)
    {
        constexpr size_t ImageAt { 24 };
        const auto area = arch::area();
        const uint32_t lo = std::max(area.lo, state.floor);
        const uint32_t room = (area.hi > (lo + SectorSize)) ? (area.hi - SectorSize - lo) : 0;
        uint32_t flash = 0;
        for (size_t index = 0; index < count; ++index) {
            flash += slotSize(applyWord(order[index], ImageAt));
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
        const uint32_t code = roundUp(applyWord(order[index], IramAt), 4);
        if (!code) {
            continue;
        }
        if (!pools.code_in_fixed || ((fixed + code) > pools.fixed)) {
            return PSTR("not enough IRAM for the module code");
        }
        state.plan_code[order[index]] = fixed;
        fixed += code;
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
    delete applying;
    applying = nullptr;
    owner = nullptr;
}

// the package just fed to its end: checked, into the directory
void applyFinish() {
    Entry entry;
    auto& up = *applying->upload;
    const auto* error = packageFinish(up, entry);
    if (!error) {
        std::memcpy(entry.name, up.header.name, sizeof(entry.name));
        entry.name[sizeof(entry.name) - 1] = '\0';
        auto& fresh = applying->fresh;
        if (fresh.count < DirectoryEntries) {
            fresh.entries[fresh.count++] = entry;
        } else {
            error = PSTR("directory full");
        }
    }

    if (error) {
        applying->error = error;
    } else {
        if (applying->done.length()) {
            applying->done += ' ';
        }
        applying->done += entry.name;
    }

    applying->upload.reset();
    applying->current = ModulesFileMax;
}

void applyData(const uint8_t* data, size_t len) {
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
            if ((at + part) == table) {
                state.error = applyPlan();
                state.fresh.magic = DirectoryMagic;
                state.fresh.count = 0;
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
                    state.upload = boxed<Upload>();
                    if (!state.upload) {
                        state.error = PSTR("no memory for the package");
                        break;
                    }
                    state.upload->apply_dir = &state.fresh;
                    state.upload->flash_floor = state.floor;
                    state.upload->ram_offset = state.plan_ram[inside];
                    state.upload->code_offset = state.plan_code[inside];
                    state.current = inside;
                }

                packageFeed(*state.upload, data, take);
                if (state.upload->error) {
                    state.error = state.upload->error;
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

// the end: nullptr - every selected package written and the directory with them in flash
const char* applyEnd(String& missing) {
    if (applying->error) {
        return applying->error;
    }

    for (size_t index = 0; index < applying->names_count; ++index) {
        const String name(applying->names[index]);
        if ((String(' ') + applying->done + ' ').indexOf(String(' ') + name + ' ') < 0) {
            missing += missing.length() ? F(", ") : F("");
            missing += name;
        }
    }
    if (missing.length()) {
        return PSTR("not in the modules file or not written");
    }

    if (!writeDirectory(applying->fresh)) {
        return PSTR("directory write");
    }

    return nullptr;
}

void onApplyData(WebRequest& request, const char*, size_t index, const uint8_t* bytes, size_t len, bool) {
    if (!webAuthenticate(request)) {
        return;
    }

    if (!index) {
        if (applying || arch::otaBusy()) {
            return;
        }

        applying = new (std::nothrow) Apply;
        if (!applying) {
            return;
        }
        owner = &request;
        auto* self = &request;
        request.onDisconnect([self]() {
            if (owner == self) {
                applyRelease();
            }
        });

        // names=a,b,c: a form field before the file (a query string is not in the URI the browser's Digest signs and
        // the server checks), or the query
        const String names = webParam(request, "names");
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

    if (applying && (owner == &request)) {
        applyData(const_cast<uint8_t*>(bytes), len);
    }
}

void onApplyDone(WebRequest& request) {
    if (!authenticated(request)) {
        return;
    }

    if (!applying || (owner != &request)) {
        answer(request, 409, F("ERROR: no apply in this request (another one, or modules running)"));
        return;
    }

    String missing;
    const auto* error = applyEnd(missing);
    if (error) {
        const int code = running ? 409 : (strcmp_P("directory write", error) == 0) ? 500 : 400;
        answer(request, code, String(F("ERROR: ")) + FPSTR(error) + (missing.length() ? (String(F(": ")) + missing) : String()));
    } else {
        answer(request, 200, String(F("OK, applied: ")) + (applying->done.length() ? applying->done : String(F("no modules")))
            + F(", restarting"));
        prepareReset(CustomResetReason::Modules);
    }

    DEBUG_MSG_P(PSTR("[MODULES] Apply: %s\n"), error ? error : PSTR("done"));
    applyRelease();
}

// the first step of an apply: no module code at the next boot (the off mark of the guard, the directory stays), the
// area free to be rewritten. The Apply writes a new directory without the mark; one that breaks off leaves the old
// modules off, MODULES.ON (the panel's "Start the modules again") brings them back
void onModulesOff(WebRequest& request) {
    if (!authenticated(request)) {
        return;
    }

    offWrite("an Apply started and did not finish: Apply again");

    answer(request, 200, F("OK, modules off, restarting"));
    prepareReset(CustomResetReason::Modules);
}

// the modules the initial image brings (a modules file at the start of the area): installed by the Apply when there is
// no directory of this head, then a restart (their statics are taken at boot). true - restarting
bool applyInitial(const Directory& dir, bool valid) {
    const auto area = arch::area();
    uint32_t head[2];
    if (!area.hi || !arch::rawRead(area.lo, head, sizeof(head)) || (head[0] != ModulesFileMagic) || !head[1]
        || (head[1] > ModulesFileMax))
    {
        return false;
    }

    // a directory with a module of this head: installed already (a module the user removed stays removed)
    for (size_t index = 0; valid && (index < dir.count); ++index) {
        Header header;
        if (readHeader(dir.entries[index], header) && (header.magic == HeaderMagic) && sameHead(header.head_md5)) {
            return false;
        }
    }

    applying = new (std::nothrow) Apply;
    if (!applying) {
        return false;
    }
    applying->every = true;

    // the file's size: the end of its last package
    uint32_t size = 8 + (head[1] * ModulesFileEntry);
    for (uint32_t index = 0; index < head[1]; ++index) {
        uint32_t row[ModulesFileEntry / 4];
        if (!arch::rawRead(area.lo + 8 + (index * ModulesFileEntry), row, sizeof(row))) {
            applyRelease();
            return false;
        }
        size = std::max(size, row[4] + row[5]);
    }
    applying->floor = area.lo + roundUp(size, SectorSize);

    alignas(4) uint8_t part[256];
    for (uint32_t done = 0; (done < size) && !applying->error; done += sizeof(part)) {
        const uint32_t length = std::min<uint32_t>(sizeof(part), size - done);
        if (!arch::rawRead(area.lo + done, part, roundUp(length, 4))) {
            applying->error = PSTR("read");
            break;
        }
        applyData(part, length);
        yield();
    }

    String missing;
    const auto* error = applyEnd(missing);
    DEBUG_MSG_P(PSTR("[MODULES] The modules of the initial image (%s): %s\n"), applying->done.c_str(),
        error ? error : PSTR("installed, restarting"));
    applyRelease();

    // the file goes: the area is the modules' now (and a directory erased later does not bring them back)
    if (!error) {
        flashErase(area.lo, roundUp(size, SectorSize));
        prepareReset(CustomResetReason::Modules);
        return true;
    }

    return false;
}


// --- web -----------------------------------------------------------------------------------------------------------

void onStatus(WebRequest& request) {
    if (!authenticated(request)) {
        return;
    }

    auto dir_box = boxed<Directory>();
    if (!dir_box) {
        request.send(503, "text/plain", "ERROR: no memory"); return;
    }
    auto& dir = *dir_box;
    const bool valid = readDirectory(dir);

    String out;
    out.reserve(1024);
    out += F("{\"version\":\"");
    out += buildApp().version.toString();
    out += F("\",\"image\":\"");
    out += F(ESPURNA_IMAGE);
    out += F("\",\"md5\":\"");
    out += headText();
    out += F("\",\"list\":");
    jsonString(out, getSetting(F("webFwUrl")).c_str());
    out += F(",\"off\":");
    jsonString(out, offReason().c_str());
    out += F(",\"modules\":[");
    for (size_t index = 0; valid && (index < dir.count); ++index) {
        const auto& entry = dir.entries[index];
        if (index) {
            out += ',';
        }
        out += F("{\"name\":");
        jsonString(out, entry.name);
        jsonField(out, F("size"), entry.size);
        jsonField(out, F("offset"), entry.offset);
        jsonField(out, F("ram"), entry.ram_size);
        jsonField(out, F("heap"), statusOf(entry).heap);
        jsonField(out, F("enabled"), (entry.flags & EntryDisabled) ? 0 : 1);
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
        jsonField(out, F("iram"), entry.code_size);
        out += F(",\"state\":\"");
        out += stateName(statusOf(entry));
        out += F("\"}");
    }
    out += ']';

    // the meters: Flash from the area's start (the head) to the directory, the arenas
    auto space_box = boxed<Spaces>();
    if (!space_box) {
        request.send(503, "text/plain", "ERROR: no memory"); return;
    }
    auto& space = *space_box;
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
        // the heap the started modules took when they started (their own allocations, not their statics), the IRAM
        // code of the modules (in the fixed pool)
        uint32_t taken = 0;
        uint32_t code = 0;
        for (size_t index = 0; valid && (index < dir.count); ++index) {
            taken += statusOf(dir.entries[index]).heap;
            code += dir.entries[index].code_size;
        }
        jsonField(out, F("ram_dynamic"), taken);
        jsonField(out, F("iram_code"), code);
    }
    // the fixed pool: the code and the statics in it
    jsonField(out, F("iram_arena"), space.fixed.hi);
    jsonField(out, F("iram_used"), space.fixed.hi - space.fixed.free());
    jsonField(out, F("iram_max"), space.fixed.largest());
    jsonField(out, F("iram_data"), space.fixed.hi);
    jsonField(out, F("iram_data_used"), space.fixed.hi - space.fixed.free());
    jsonField(out, F("iram_data_max"), space.fixed.largest());
    {
        arch::Pools pools {};
        arch::pools(pools);
        if (pools.name) {
            out += F(",\"pool\":\"");
            out += pools.name;
            out += '"';
        }
    }

    arch::statusJson(out);
    jsonField(out, F("heap"), systemFreeHeap());
    jsonField(out, F("heap_boot"), systemInitialFreeHeap());
    jsonField(out, F("oom"), arch::failedAllocations());
    out += '}';

    webSend(request, 200, "application/json", out);
}

void onModulesOn(WebRequest& request) {
    if (!authenticated(request)) {
        return;
    }

    modulesOffClear();
    request.send(200, "text/plain", "OK, modules on, restarting");
    prepareReset(CustomResetReason::Modules);
}

// a module out of the directory; its code keeps running until the restart
void onRemove(WebRequest& request) {
    if (!authenticated(request)) {
        return;
    }

    const char* param = request.param("name");
    if (!param) {
        request.send(400, "text/plain", "ERROR: no name");
        return;
    }

    const char* error = nullptr;
    if (!removeEntry(String(param), error)) {
        answer(request, 400, String(F("ERROR: ")) + FPSTR(error));
        return;
    }

    request.send(200, "text/plain", "OK, removed, restart to apply");
}

// POST name=, on=1|0
void onEnable(WebRequest& request) {
    if (!authenticated(request)) {
        return;
    }

    const char* name = request.param("name");
    const char* on = request.param("on");
    if (!name || !on) {
        request.send(400, "text/plain", "ERROR: name and on");
        return;
    }

    String text;
    const auto* error = setEnabled(String(name), strcmp(on, "1") == 0, text);
    if (error) {
        answer(request, 400, String(F("ERROR: ")) + FPSTR(error));
        return;
    }

    answer(request, 200, text);
}

void onRestart(WebRequest& request) {
    if (!authenticated(request)) {
        return;
    }

    request.send(200, "text/plain", "OK");
    prepareReset(CustomResetReason::Modules);
}

void onPage(WebRequest& request) {
    if (!authenticated(request)) {
        return;
    }

    headPage(request);
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

    auto space = boxed<Spaces>();
    if (!space) {
        terminalError(ctx, F("no memory"));
        return;
    }
    spaces(dir, valid, *space);
    ctx.output.printf_P(PSTR("free for modules: flash %u (largest %u), heap block %u of %u, fixed pool %u of %u bytes\n"),
        space->flash.free(), space->flash.largest(), space->ram.hi - space->ram.free(), space->ram.hi,
        space->fixed.hi - space->fixed.free(), space->fixed.hi);

    if (valid) {
        for (size_t index = 0; index < dir.count; ++index) {
            const auto& entry = dir.entries[index];
            const auto status = statusOf(entry);
            ctx.output.printf_P(PSTR("%-15s 0x%06x %6u bytes, RAM %u @%u%s, code %u, bases %08x %08x %08x: %s%s%s\n"),
                entry.name, entry.offset, entry.size, entry.ram_size, entry.ram_offset & ~RamSecondArena,
                (entry.ram_offset & RamSecondArena) ? " (fixed pool)" : "", entry.code_size,
                entry.bases[BaseFlash], entry.bases[BaseCode], entry.bases[BaseRam],
                String(stateName(status)).c_str(), status.reason ? " - " : "",
                status.reason ? String(FPSTR(status.reason)).c_str() : "");
        }
    } else {
        ctx.output.println(F("no modules"));
    }

    terminalOK(ctx);
}

// the modules off from the next boot (the off mark, as /modules/off): they stay installed, MODULES.ON brings them back.
// Works from the unstable mode too (telnet over the board's own access point)
void commandOff(::terminal::CommandContext&& ctx) {
    modulesOffMark("MODULES.OFF from the terminal");
    terminalOK(ctx);
    prepareReset(CustomResetReason::Modules);
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
    guardMark();
    espurnaRegisterLoop(guardLoop);
    guardTimer();
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
namespace espurna {
namespace modules {
namespace {

// Boots cut short, for the light switch of a board in a wall: a pair of words per boot in the last quarter of the
// directory sector (the Beken guard's marks are at 2048..2560), written without an erase (bits only go 1 -> 0):
// [0] cleared at boot, [1] once the boot ran PowerSeconds or when the next boot was a restart ESPurna asked for.
// PowerQuick boots in a row that never got to [1] (switched on and off again, or crashing right away): the modules
// stay off (the off mark), the board comes up with the network. A directory write erases the sector: the count
// starts anew, nothing lost. Full: the directory is written again into that sector
constexpr uint32_t PowerMarksOffset { 3072 };
constexpr size_t PowerMarks { 128 };
constexpr size_t PowerQuick { 3 };
constexpr espurna::duration::Seconds PowerSeconds { 10 };

constexpr uint32_t MarkFree { 0xffffffff };

espurna::timer::SystemTimer power_timer;
uint32_t power_at { 0 };

bool powerZero(uint32_t at) {
    const uint32_t zero = 0;
    return arch::rawWrite(at, &zero, sizeof(zero));
}

void powerSurvived() {
    if (power_at) {
        powerZero(power_at + 4);
    }
}

void powerMarks() {
    const auto area = arch::area();
    if (!area.hi) {
        return;
    }

    // nothing installed: nothing to keep off
    auto dir = boxed<Directory>();
    if (!dir || !readDirectory(*dir) || !dir->count) {
        return;
    }

    std::unique_ptr<uint32_t[]> marks(new (std::nothrow) uint32_t[PowerMarks * 2]);
    if (!marks || !arch::rawRead(area.hi + PowerMarksOffset, marks.get(), PowerMarks * 2 * sizeof(uint32_t))) {
        return;
    }

    size_t index = 0;
    while ((index < PowerMarks) && (marks[index * 2] != MarkFree)) {
        ++index;
    }

    if (index == PowerMarks) {
        if (!writeDirectoryPrimary(*dir)) {
            return;
        }
        index = 0;
    }

    const uint32_t base = area.hi + PowerMarksOffset;
    const bool requested = customResetReason() != CustomResetReason::None;
    if (requested && index) {
        // the boot before ended by a restart ESPurna asked for, not cut
        powerZero(base + ((index - 1) * 8) + 4);
    }

    size_t quick = 0;
    for (size_t before = index; !requested && before; --before) {
        if ((marks[(before - 1) * 2] == 0) && (marks[((before - 1) * 2) + 1] == MarkFree)) {
            ++quick;
        } else {
            break;
        }
    }

    powerZero(base + (index * 8));
    power_at = base + (index * 8);
    power_timer.once(PowerSeconds, powerSurvived);

    if ((quick + 1) >= PowerQuick) {
        DEBUG_MSG_P(PSTR("[MODULES] %u short boots in a row\n"), static_cast<unsigned>(quick + 1));
        modulesOffMark("the power was switched on 3 times within seconds");
    }
}

} // namespace
} // namespace modules
} // namespace espurna

bool modulesOffMark(const char* reason) {
    using namespace espurna::modules;
    auto dir_box = boxed<Directory>();
    if (!dir_box || !readDirectory(*dir_box) || !dir_box->count || dir_box->off[0]) {
        return false;
    }

    offWrite(reason);
    DEBUG_MSG_P(PSTR("[MODULES] Off at the next boot: %s\n"), reason);
    return true;
}

void modulesBootSetup() {
    using namespace espurna::modules;

    arch::setup();

#if TERMINAL_SUPPORT
    espurna::terminal::add(Commands);
#endif

    guardBoot();

    powerMarks();

#if SYSTEM_CHECK_ENABLED
    // the step before the unstable mode: this boot follows a crash and one more makes the board unstable (its own
    // access point, no network). The modules stay off from here: the board keeps the network, the WebUI tells why,
    // "Start the modules again" or an Apply brings them back
    // The count starts anew with the modules off: two more crashes without them before the unstable mode, else the
    // next crash went straight there and the step without modules never came (ESP32 10.0.5.66, 11-11)
    if (crashRestart() && (systemStabilityCounter() >= SYSTEM_CHECK_MAX)
        && modulesOffMark("the board crashed at boot again and again"))
    {
        systemForceStable();
    }
#endif

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

    auto& server = webServer();
    server.on("/modules/apply", WebMethod::Post, onApplyDone, onApplyData);
    server.on("/modules/off", WebMethod::Post, onModulesOff);
    server.on("/modules/restart", WebMethod::Post, onRestart);
    server.on("/modules/remove", WebMethod::Post, onRemove);
    server.on("/modules/on", WebMethod::Post, onModulesOn);
    server.on("/modules/enable", WebMethod::Post, onEnable);
    server.on("/modules.html", WebMethod::Get, onPage);
    server.on("/modules", WebMethod::Get, onStatus);

    // the MODULES menu entry of the WebUI module
    wsRegister().onVisible([](JsonObject& root) {
        wsPayloadModule(root, STRING_VIEW("mod"));
    });

    Directory dir;
    const bool valid = readDirectory(dir);
    if (applyInitial(dir, valid) || !valid) {
        return;
    }

    if (offReason().length()) {
        for (size_t index = 0; index < dir.count; ++index) {
            status[index] = Status{State::Off, nullptr, dir.entries[index].offset, dir.entries[index].crc32};
        }
        DEBUG_MSG_P(PSTR("[MODULES] Off (%s), MODULES.ON starts them again\n"), offReason().c_str());
        return;
    }

    // the bases of a module moved since the Apply (the maps of the flash, the heap block): rewritten for this boot
    {
        arch::Pools pools {};
        arch::pools(pools);
        bool changed = false;
        for (size_t index = 0; index < dir.count; ++index) {
            auto& entry = dir.entries[index];
            uint32_t now[3];
            basesNow(entry, pools, now);
            if ((entry.flags & EntryDisabled) || (std::memcmp(now, entry.bases, sizeof(now)) == 0)) {
                continue;
            }

            Header header;
            if (checkImage(entry, header)) {
                continue;
            }

            const auto* error = relocateEntry(entry, now);
            DEBUG_MSG_P(PSTR("[MODULES] %s relocated for this boot%s%s\n"), entry.name,
                error ? ": " : "", error ? error : "");
            changed = changed || !error;
        }
        if (changed && !writeDirectory(dir)) {
            DEBUG_MSG_P(PSTR("[MODULES] Directory write failed\n"));
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
    for (size_t index = 0; index < dir.count; ++index) {
        const auto& entry = dir.entries[index];
        if (entry.flags & EntryDisabled) {
            status[index] = Status{State::Disabled, nullptr, entry.offset, entry.crc32};
            std::memcpy(status[index].name, entry.name, sizeof(status[index].name));
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
            startEntry(dir.entries[index]);
        }
        if (!started_any) {
            break;
        }
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

String modulesHeadId() {
    return espurna::modules::headText();
}

bool modulesRunning() {
    return espurna::modules::running;
}

bool modulesEnabled(const char* name) {
    using namespace espurna::modules;
    std::unique_ptr<Directory> dir(new (std::nothrow) Directory);
    // the modules off (the guard, an Apply that broke off): none of them starts this boot
    if (!dir || !readDirectory(*dir) || dir->off[0]) {
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

void modulesOnHome(WebRequest& request) {
    using namespace espurna::modules;
    if (!authenticated(request)) {
        return;
    }

    for (size_t index = 0; index < data_count; ++index) {
        if (std::strcmp(data[index].name, "webui") == 0) {
            sendFlash(request, reinterpret_cast<const uint8_t*>(data[index].data), data[index].size, true);
            return;
        }
    }

    headPage(request);
}

void modulesForget() {
    espurna::modules::eraseDirectory();
}

#endif // MODULES_SUPPORT
