/*

PROTOTYPE: the ESP32 backend of the module loader (docs/modules.md, "ESP32"; the core is modules.cpp, the contract
modules_core.h)

Flash: the modules lie in the tail of app0 behind the head (or in a data partition named `modules`), placed from the top
down under the directory (the last sector). The whole area is mapped twice at the first directory read (in
modulesBootSetup, before anything else of ESPurna maps flash for good), into the instruction bus (code) and the data
bus (header, rodata, the .data image), and the maps stay. RAM: one arena from the heap, as big as the installed modules
need, allocated before they start.

A module is relocated for the addresses it will have: the data view of its slot (kind 1), the RAM arena (kind 2), the
instruction view of its slot (kind 3). Each entry keeps the bases its words hold (arch[]); when the maps or the arena
come out elsewhere at boot, the module is relocated again in place from its relocation maps (kept behind the image).

*/

#include "espurna.h"
#include "modules.h"

#if MODULES_SUPPORT && MODULES_CORE && defined(ESP32)

#include "modules_core.h"

#include <esp_attr.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <spi_flash_mmap.h>

// esp_private/cache_utils.h: drops the cached lines of the mapped pages of a flash range (a module relocated in place
// while the others run from the same maps)
extern "C" bool spi_flash_check_and_flush_cache(size_t start_addr, size_t length);

#include <algorithm>
#include <cstring>
#include <memory>

// heap the head keeps for itself (Wi-Fi, the web server, TLS) when the Apply sizes the modules' heap block
#ifndef MODULES_HEAP_KEEP
#define MODULES_HEAP_KEEP 32768
#endif

namespace espurna {
namespace modules {
namespace {

constexpr uint32_t PageSize { 0x10000 };
constexpr uint32_t DirectoryMagic { 0x35524445 }; // "EDR5": ESP32, the core's entry with the bases in arch[], 16 entries
constexpr uint32_t PackageVersion { 2 };          // ESP32 kinds: 1 data view, 2 RAM, 3 instruction view

// Entry::arch[]: the bases the module's words hold now
constexpr size_t BaseDrom { 0 };    // data view of the slot
constexpr size_t BaseIrom { 1 };    // instruction view of the slot
constexpr size_t BaseRam { 2 };     // its RAM (0 + ram_offset when there was no arena at the install)

uint32_t mapsSize(uint32_t image) {
    return ((image + ChunkSize - 1) / ChunkSize) * MapSize;
}

// flash taken by a module: the image and its relocation maps, each from a sector
uint32_t slotSize(uint32_t image) {
    return roundUp(image, SectorSize) + roundUp(mapsSize(image), SectorSize);
}

bool readFlash(uint32_t address, void* data, uint32_t size) {
    return esp_flash_read(nullptr, data, address, size) == ESP_OK;
}

// --- the area and its two maps ------------------------------------------------------------------

struct Region {
    const esp_partition_t* part { nullptr };    // app0 (its tail) or `modules`
    uint32_t start { 0 };       // flash address where the Flash meter starts (app0: the head)
    uint32_t lo { 0 };          // first byte for modules (64 KB aligned: the start of the maps)
    uint32_t hi { 0 };          // the directory
    uint32_t irom { 0 };        // instruction view of `lo`
    uint32_t drom { 0 };        // data view of `lo`
    spi_flash_mmap_handle_t irom_handle { 0 };
    spi_flash_mmap_handle_t drom_handle { 0 };
    bool tried { false };
    bool mapped { false };
};

Region region;

uint8_t* arena { nullptr };
uint32_t arena_size { 0 };

// end of the app image in app0, from its segment headers (esp_image_verify() would read and hash all of it)
uint32_t headSize(const esp_partition_t* app) {
    esp_image_header_t header;
    if (!readFlash(app->address, &header, sizeof(header))
        || (header.magic != ESP_IMAGE_HEADER_MAGIC) || (header.segment_count > ESP_IMAGE_MAX_SEGMENTS))
    {
        return 0;
    }

    uint32_t pos = sizeof(header);
    for (uint8_t index = 0; index < header.segment_count; ++index) {
        esp_image_segment_header_t segment;
        if (!readFlash(app->address + pos, &segment, sizeof(segment))) {
            return 0;
        }
        pos += sizeof(segment) + segment.data_len;
    }

    // checksum byte at the end of a 16 byte block, then the SHA-256 when appended
    pos = roundUp(pos + 1, 16);
    if (header.hash_appended) {
        pos += 32;
    }

    return pos;
}

bool findArea() {
    const auto* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "modules");
    if (part) {
        region.part = part;
        region.start = part->address;
        region.lo = part->address;
        region.hi = part->address + part->size - SectorSize;
        return region.hi > region.lo;
    }

    const auto* app = esp_ota_get_running_partition();
    if (!app) {
        return false;
    }

    const auto head = headSize(app);
    if (!head) {
        return false;
    }

    region.part = app;
    region.start = app->address;
    region.lo = roundUp(app->address + head, PageSize);
    region.hi = app->address + app->size - SectorSize;
    return region.hi > region.lo;
}

bool mapArea() {
    const void* ptr { nullptr };
    const auto offset = region.lo - region.part->address;
    const auto size = region.hi - region.lo;
    if (esp_partition_mmap(region.part, offset, size, ESP_PARTITION_MMAP_INST, &ptr, &region.irom_handle) != ESP_OK) {
        return false;
    }
    region.irom = reinterpret_cast<uint32_t>(ptr);

    if (esp_partition_mmap(region.part, offset, size, ESP_PARTITION_MMAP_DATA, &ptr, &region.drom_handle) != ESP_OK) {
        spi_flash_munmap(region.irom_handle);
        return false;
    }
    region.drom = reinterpret_cast<uint32_t>(ptr);

    region.mapped = true;
    return true;
}

void unmapArea() {
    if (region.mapped) {
        spi_flash_munmap(region.drom_handle);
        spi_flash_munmap(region.irom_handle);
        region.mapped = false;
    }
}

// the first call maps the area: arch::setup(), first thing in modulesBootSetup()
bool areaReady() {
    if (!region.tried) {
        region.tried = true;
        if (!findArea() || !mapArea()) {
            DEBUG_MSG_P(PSTR("[MODULES] No module area\n"));
        }
    }

    return region.part != nullptr;
}

uint32_t dromOf(uint32_t offset) {
    return region.drom + (offset - region.lo);
}

uint32_t iromOf(uint32_t offset) {
    return region.irom + (offset - region.lo);
}

// --- flash access -------------------------------------------------------------------------------

// esp_flash_write() aborts on any write into the running app partition (DANGEROUS_WRITE_ABORTS of the Arduino
// sdkconfig). Same chip, without that check; the core writes only inside the module area and the directory sector
esp_flash_os_functions_t unprotected_os;
esp_flash_t unprotected_chip;

esp_flash_t* writableChip() {
    static bool done { false };
    if (!done) {
        unprotected_os = *esp_flash_default_chip->os_func;
        unprotected_os.region_protected = nullptr;
        unprotected_chip = *esp_flash_default_chip;
        unprotected_chip.os_func = &unprotected_os;
        done = true;
    }

    return &unprotected_chip;
}

// --- relocation at boot: the maps or the arena moved -------------------------------------------

// adds the deltas to the words of each kind, sector by sector; the new CRC into the entry
const char* relocate(Entry& entry, const uint32_t delta[4]) {
    std::unique_ptr<uint8_t[]> sector(new (std::nothrow) uint8_t[SectorSize]);
    if (!sector) {
        return PSTR("no memory to relocate");
    }

    uint8_t map[MapSize];
    const uint32_t maps = entry.offset + roundUp(entry.size, SectorSize);
    uint32_t crc = 0;
    for (uint32_t done = 0; done < entry.size; done += ChunkSize) {
        const uint32_t size = std::min<uint32_t>(ChunkSize, entry.size - done);
        if (!readFlash(entry.offset + done, sector.get(), size)
            || !readFlash(maps + (done / ChunkSize) * MapSize, map, MapSize))
        {
            return PSTR("relocate: read");
        }

        for (uint32_t index = 0; index < size / 4; ++index) {
            const uint8_t kind = (map[index >> 2] >> ((index & 3) * 2)) & 3;
            if (kind && delta[kind]) {
                uint32_t value;
                std::memcpy(&value, &sector[index * 4], 4);
                value += delta[kind];
                std::memcpy(&sector[index * 4], &value, 4);
            }
        }

        if (!flashErase(entry.offset + done, SectorSize) || !flashWrite(entry.offset + done, sector.get(), size)) {
            return PSTR("relocate: write");
        }
        crc = crc32Update(crc, sector.get(), size);
    }

    entry.crc32 = crc;
    return nullptr;
}

// RAM of the modules switched on in this boot that the arena (sized at boot) could not hold: a heap block each, until
// the next boot puts them into the arena (relocateMoved)
struct RamBlock {
    uint32_t lo;
    uint32_t hi;
};

RamBlock ram_blocks[DirectoryEntries];
size_t ram_blocks_count { 0 };

bool inRamBlock(uint32_t address, uint32_t size) {
    for (size_t index = 0; index < ram_blocks_count; ++index) {
        if ((address >= ram_blocks[index].lo) && ((address + size) <= ram_blocks[index].hi)) {
            return true;
        }
    }

    return false;
}

bool inArena(const Entry& entry) {
    return arena && ((entry.ram_offset + entry.ram_size) <= arena_size);
}

// the bases the module needs in this boot
void bases(const Entry& entry, uint32_t& drom, uint32_t& irom, uint32_t& ram) {
    drom = dromOf(entry.offset);
    irom = iromOf(entry.offset);
    if (!entry.ram_size) {
        ram = entry.arch[BaseRam];
    } else if (!inArena(entry) && inRamBlock(entry.arch[BaseRam], entry.ram_size)) {
        ram = entry.arch[BaseRam];
    } else {
        ram = reinterpret_cast<uint32_t>(arena) + entry.ram_offset;
    }
}

// the arena is the core's heap block (modulesRamSetup), as big as the installed modules need: the Apply placed them
// for its address, a module whose address moved anyway is relocated from its maps (relocateMoved)
void allocateArena(const Directory& dir) {
    if (arena || !heapBlockSize() || (heapBlockSize() < heapNeed(dir))) {
        return;
    }

    arena = reinterpret_cast<uint8_t*>(heapBlockBase());
    arena_size = heapBlockSize();
}

// every module of this head whose bases moved: relocated in place, the directory updated. Then the maps again, so
// the cache cannot hold the old words
void relocateMoved(Directory& dir) {
    bool changed = false;
    for (size_t index = 0; index < dir.count; ++index) {
        auto& entry = dir.entries[index];
        // no RAM: arch::place relocates it when it is switched on. Running: its words are what its code uses now
        if ((entry.flags & EntryUnplaced) || startedAt(entry.offset)) {
            continue;
        }

        uint32_t drom, irom, ram;
        bases(entry, drom, irom, ram);
        if ((drom == entry.arch[BaseDrom]) && (irom == entry.arch[BaseIrom]) && (ram == entry.arch[BaseRam])) {
            continue;
        }

        if (entry.ram_size && !arena) {
            continue;
        }

        Header header;
        if (checkImage(entry, header)) {
            continue;
        }

        const uint32_t delta[4] {
            0, drom - entry.arch[BaseDrom], ram - entry.arch[BaseRam], irom - entry.arch[BaseIrom]};
        const auto* error = relocate(entry, delta);
        DEBUG_MSG_P(PSTR("[MODULES] %s relocated: drom %08x -> %08x, irom %08x -> %08x, ram %08x -> %08x%s%s\n"),
            entry.name, entry.arch[BaseDrom], drom, entry.arch[BaseIrom], irom, entry.arch[BaseRam], ram,
            error ? ": " : "", error ? error : "");
        if (!error) {
            entry.arch[BaseDrom] = drom;
            entry.arch[BaseIrom] = irom;
            entry.arch[BaseRam] = ram;
            spi_flash_check_and_flush_cache(entry.offset, entry.size);
        }
        changed = true;
    }

    if (!changed) {
        return;
    }

    writeDirectory(dir);

    // with module code running the maps stay (it runs from them): the cached lines were dropped above
    if (startedCount()) {
        return;
    }

    const auto irom = region.irom;
    const auto drom = region.drom;
    unmapArea();
    if (!mapArea() || (region.irom != irom) || (region.drom != drom)) {
        DEBUG_MSG_P(PSTR("[MODULES] the maps moved again (irom %08x, drom %08x)\n"), region.irom, region.drom);
    }
}

// --- the guard -----------------------------------------------------------------------------------

// survives a crash restart, not a power cut
RTC_NOINIT_ATTR GuardSaved guard_memory;

esp_timer_handle_t guard_timer { nullptr };

void guardTimerCallback(void*) {
    guardTick();
}

// --- upload: placed when its header is in, relocated word by word as it arrives -------------------------------------

struct Upload : WordUpload {
    uint32_t ram_offset { 0 };
    uint32_t bases[3] {};       // drom, irom, ram the words will hold
};

const char* prepare(PackageReader& reader) {
    auto& up = static_cast<Upload&>(reader);
    const auto& header = up.header;
    if (!region.mapped) {
        return PSTR("no module area (app0 tail) mapped");
    }

    Directory dir;
    bool installed = false;
    const auto* error = packageAccept(up, PackageVersion, dir, installed);
    if (error) {
        return error;
    }

    if (header.iram_size) {
        return PSTR("no IRAM arena on the ESP32 head");
    }

    // a new copy of an installed module: the arena is filled at boot, its RAM is free for the new copy
    Spaces space;
    spaces(dir, true, space, installed ? header.name : nullptr);
    if (!space.flash.place(slotSize(header.image_size), true, up.slot)) {
        return PSTR("not enough flash");
    }

    const uint32_t ram_size = roundUp(header.ram_size, 8);
    if (ram_size && up.planned) {
        if (up.planned_ram & RamSecondArena) {
            return PSTR("no fixed RAM pool on the ESP32");
        }
        up.ram_offset = up.planned_ram;
    } else if (ram_size) {
        if (!space.ram.place(ram_size, false, up.ram_offset)) {
            return PSTR("not enough RAM in the arena");
        }
    } else {
        up.ram_offset = 0;
    }

    // the maps behind the image: erased now, written chunk by chunk as they arrive
    const uint32_t maps = up.slot + roundUp(header.image_size, SectorSize);
    if (!flashErase(maps, roundUp(mapsSize(header.image_size), SectorSize))) {
        return PSTR("flash erase");
    }

    // the Apply: for the heap block the next boot takes (its address now, the boot relocates when it moved). Else in
    // this boot's arena when it fits there, else for base 0: the next boot sizes the arena and relocates it
    const bool in_arena = arena && ((up.ram_offset + ram_size) <= arena_size);
    up.bases[0] = dromOf(up.slot);
    up.bases[1] = iromOf(up.slot);
    up.bases[2] = (up.planned ? heapBlockBase() : (in_arena ? reinterpret_cast<uint32_t>(arena) : 0))
        + up.ram_offset;
    up.delta[0] = 0;
    up.delta[1] = up.bases[0] - header.link_flash;
    up.delta[2] = up.bases[2] - header.link_ram;
    up.delta[3] = up.bases[1] - header.link_iram;

    return nullptr;
}

// image bytes of the current chunk: its map is kept behind the image (for a relocation at boot), the words relocated
// by it as they come
const char* bytes(PackageReader& reader, const uint8_t* data, size_t size) {
    auto& up = static_cast<Upload&>(reader);
    if ((up.delivered % ChunkSize) == 0) {
        const uint32_t maps = up.slot + roundUp(up.header.image_size, SectorSize);
        if (!flashWrite(maps + ((up.delivered / ChunkSize) * MapSize), up.map, MapSize)) {
            return PSTR("flash write");
        }
    }

    return wordBytes(reader, data, size);
}

constexpr PackageHooks Hooks { prepare, bytes };

} // namespace

namespace arch {

// first thing of modulesBootSetup(): the maps, before anything else of ESPurna maps flash for good
void setup() {
    areaReady();
}

uint32_t directoryMagic() {
    return DirectoryMagic;
}

// the Flash meter from the start of app0 (the head) to the directory
Area area() {
    if (!areaReady()) {
        return Area{0, 0, 0};
    }

    return Area{region.start, region.lo, region.hi};
}

bool rawRead(uint32_t offset, void* data, size_t size) {
    return readFlash(offset, data, size);
}

bool rawErase(uint32_t offset, uint32_t size) {
    return esp_flash_erase_region(writableChip(), offset, size) == ESP_OK;
}

bool rawWrite(uint32_t offset, const void* data, size_t size) {
    return esp_flash_write(writableChip(), data, offset, size) == ESP_OK;
}

// MD5 of the running app image (= firmware.bin), as the module link took it from the head
bool headId(uint8_t out[16]) {
    return hexId(ESP.getSketchMD5(), out);
}

// header, rodata and the .data image from the data view of the slot, the code from its instruction view (the header's
// iram_start..iram_end, not copied), RAM in the arena or a heap block of this boot
const char* runs(const Entry& entry, const Header& header, Runs& out) {
    uint32_t drom, irom, ram;
    bases(entry, drom, irom, ram);
    if ((drom != entry.arch[BaseDrom]) || (irom != entry.arch[BaseIrom]) || (ram != entry.arch[BaseRam])) {
        return PSTR("not relocated for this boot");
    }

    const bool has_ram = (header.data_end != header.data_start) || (header.bss_end != header.bss_start);
    if (has_ram && !inArena(entry) && !inRamBlock(ram, entry.ram_size)) {
        return PSTR("no RAM arena");
    }

    out.flash_lo = drom;
    out.flash_hi = drom + entry.size;
    out.entry_lo = irom;
    out.entry_hi = irom + entry.size;
    out.ram_lo = ram;
    out.ram_hi = ram + entry.ram_size;
    out.iram_lo = irom;
    out.iram_hi = irom + entry.size;
    out.iram_copy = false;
    out.words = false;
    return nullptr;
}

// a module without RAM switched on: its RAM now (the arena of this boot, else a heap block until the next boot), its
// image relocated in its slot for the maps and that RAM (from its relocation maps behind the image), the cached lines
// of the slot dropped. The core then starts it, from the loop, no restart
const char* place(const Directory& dir, Entry& entry) {
    if (!region.mapped) {
        return PSTR("no module area mapped");
    }

    Header header;
    const auto* error = checkImage(entry, header);
    if (error) {
        return error;
    }

    uint32_t ram_offset = entry.ram_offset;
    uint32_t ram = entry.arch[BaseRam];
    if (entry.ram_size) {
        Spaces space;
        spaces(dir, true, space, entry.name);
        if (!space.ram.place(entry.ram_size, false, ram_offset)) {
            return PSTR("not enough RAM in the arena");
        }

        if (arena && ((ram_offset + entry.ram_size) <= arena_size)) {
            ram = reinterpret_cast<uint32_t>(arena) + ram_offset;
        } else if (ram_blocks_count < DirectoryEntries) {
            auto* block = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, entry.ram_size,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            if (!block) {
                return PSTR("no heap for its RAM");
            }
            ram = reinterpret_cast<uint32_t>(block);
            ram_blocks[ram_blocks_count++] = RamBlock{ram, ram + entry.ram_size};
        } else {
            return PSTR("no heap for its RAM");
        }
    }

    const uint32_t drom = dromOf(entry.offset);
    const uint32_t irom = iromOf(entry.offset);
    const uint32_t delta[4] {
        0, drom - entry.arch[BaseDrom], ram - entry.arch[BaseRam], irom - entry.arch[BaseIrom]};
    if (delta[1] || delta[2] || delta[3]) {
        error = relocate(entry, delta);
        if (error) {
            return error;
        }
        spi_flash_check_and_flush_cache(entry.offset, entry.size);
    }

    entry.ram_offset = ram_offset;
    entry.arch[BaseDrom] = drom;
    entry.arch[BaseIrom] = irom;
    entry.arch[BaseRam] = ram;
    DEBUG_MSG_P(PSTR("[MODULES] %s placed: RAM %u at %08x%s\n"), entry.name, entry.ram_size, ram,
        inArena(entry) ? "" : " (heap block until the next boot)");
    return nullptr;
}

// the image and its relocation maps stay in its slot: placed again at any time, no download
bool placeable(const Entry& entry) {
    return entry.size && (entry.slot >= slotSize(entry.size));
}

void beforeStart(Directory& dir, bool valid) {
    if (!valid || !region.mapped) {
        return;
    }

    // at boot, before the first module starts: later (a module switched on) the arena stays as it is, a module that
    // does not fit it gets a heap block (arch::place)
    if (!startedCount()) {
        allocateArena(dir);
    }
    relocateMoved(dir);
}

PackageReader* uploadNew(AsyncWebServerRequest*) {
    auto* out = new (std::nothrow) Upload();
    if (out) {
        out->hooks = &Hooks;
    }

    return out;
}

const char* uploadFinish(PackageReader& reader, Entry& out) {
    auto& up = static_cast<Upload&>(reader);
    uint32_t crc;
    const auto* error = wordFinish(up, crc);
    if (error) {
        return error;
    }

    out.offset = up.slot;
    out.slot = slotSize(up.written);
    out.size = up.written;
    out.crc32 = crc;
    out.ram_offset = up.ram_offset;
    out.ram_size = roundUp(up.header.ram_size, 8);
    out.arch[BaseDrom] = up.bases[0];
    out.arch[BaseIrom] = up.bases[1];
    out.arch[BaseRam] = up.bases[2];
    return nullptr;
}

uint32_t slotFor(uint32_t image) {
    return slotSize(image);
}

// heap only: RTC FAST is reachable from the PRO CPU alone (the loop runs on the APP CPU), the IRAM tail takes 32-bit
// access only and holds no IRAM_ATTR code of modules here
void pools(Pools& out) {
    const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    out.heap = heapBlockSize() + ((largest > MODULES_HEAP_KEEP) ? (largest - MODULES_HEAP_KEEP) : 0);
    out.fixed = 0;
    out.fixed_words = false;
    out.code_in_fixed = false;
}

void statusJson(String& out) {
    jsonField(out, F("chip"), ESP.getFlashChipSize());
}

void listHead(Print& out) {
    out.printf_P(PSTR("loader esp32 rev 3 (core), %s 0x%06x, maps: irom %08x, drom %08x%s; RAM arena %u bytes at %p\n"),
        region.part ? region.part->label : "-", region.part ? region.part->address : 0,
        region.irom, region.drom, region.mapped ? "" : " (not mapped)", arena_size, arena);
}

void listEntry(Print& out, const Entry& entry) {
    out.printf_P(PSTR(", bases %08x %08x %08x"), entry.arch[BaseDrom], entry.arch[BaseIrom], entry.arch[BaseRam]);
}

bool crashRestart() {
    switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        return true;
    default:
        break;
    }

    return false;
}

GuardState& guardState() {
    return guardStateIn(guard_memory);
}

// esp_timer task: runs while loop() waits or hangs
void guardTimer() {
    if (!guard_timer) {
        esp_timer_create_args_t args {};
        args.callback = guardTimerCallback;
        args.name = "modules";
        if (esp_timer_create(&args, &guard_timer) != ESP_OK) {
            guard_timer = nullptr;
            return;
        }
    }

    esp_timer_start_periodic(guard_timer, 1000 * 1000);
}

void guardTimerStop() {
    if (guard_timer) {
        esp_timer_stop(guard_timer);
    }
}

void restartNow() {
    esp_restart();
}

uint32_t freeHeap() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

} // namespace arch
} // namespace modules
} // namespace espurna

#endif // MODULES_SUPPORT && MODULES_CORE && ESP32
