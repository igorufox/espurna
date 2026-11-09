/*

PROTOTYPE: module loader of the ESP8266 1 MB head image (docs/esp8266-modules.md, modules.h)

Flash: modules lie under the directory sector (the last sector of the sketch area), placed from the top down; the
head grows from 0x0 up. RAM / IRAM: modules take consecutive pieces of the two arenas below.

*/

#include "espurna.h"

#if MODULES_SUPPORT

#include "espurna_ota.h"
#include "storage_eeprom.h"
#include "modules.h"
#include "terminal.h"
#include "web.h"
#include "ws.h"

#include <Updater.h>
#include <spi_flash.h>

#include <cstring>
#include <memory>

#ifndef MODULES_RAM_ARENA
#define MODULES_RAM_ARENA 5120
#endif

#ifndef MODULES_IRAM_ARENA
#define MODULES_IRAM_ARENA 2048
#endif

// guard: free heap below this for MODULES_HEAP_SECONDS while modules run -> modules off, restart
#ifndef MODULES_HEAP_MIN
#define MODULES_HEAP_MIN 2048
#endif

#ifndef MODULES_HEAP_SECONDS
#define MODULES_HEAP_SECONDS 5
#endif

// guard: this many boots in a row crashed with the modules running, before MODULES_BOOT_SECONDS -> modules off
#ifndef MODULES_CRASHES
#define MODULES_CRASHES 3
#endif

#ifndef MODULES_BOOT_SECONDS
#define MODULES_BOOT_SECONDS 60
#endif

extern "C" {

// the module link reads the size of both from the head ELF (nm -S)
__attribute__((used, aligned(4)))
uint8_t espurna_modules_ram_arena[MODULES_RAM_ARENA];

// writable code memory: only 32 bit stores. Zero filled in the image (part of the .text1 segment)
__attribute__((used, aligned(4), section(".iram.text.espurna_modules_iram_arena")))
uint32_t espurna_modules_iram_arena[MODULES_IRAM_ARENA / 4] = {};

extern uint32_t _FS_start;

} // extern "C"

namespace espurna {
namespace modules {
namespace {

constexpr uint32_t FlashMapped { 0x40200000 };
constexpr uint32_t SectorSize { 4096 };
constexpr uint32_t DirectoryMagic { 0x32524445 }; // "EDR2": + the guard's reason
constexpr size_t DirectoryEntries { 12 };

struct Entry {
    char name[16];
    uint32_t offset;    // flash offset of the module (its header)
    uint32_t size;      // image bytes
    uint32_t crc32;     // of the image as written (relocated)
    uint32_t ram_offset;
    uint32_t ram_size;
    uint32_t iram_offset;
    uint32_t iram_size;
};

struct Directory {
    uint32_t magic;
    uint32_t count;
    Entry entries[DirectoryEntries];
    // the guard: not empty - the modules stay off, why. Here and not in the settings: EEPROM_Rotate went back to an
    // older copy of the settings after the crashes and the unstable mode, and the modules started again
    char off[96];
};

static_assert(sizeof(Directory) <= SectorSize, "");

enum class State {
    None,
    Skipped,    // unstable mode
    Off,        // the guard: setting modulesOff keeps the modules from starting
    Started,
    Stale,      // made for another head image
    Broken,     // bad header / crc / ranges
};

struct Status {
    State state { State::None };
    const char* reason { nullptr };
    uint32_t offset { 0 };  // of the entry at boot: the directory changes before the restart, the indexes move
    uint32_t crc32 { 0 };   // ... and a new copy may take the place of one that did not start
};

Status status[DirectoryEntries];

// what this boot did with the module at this flash offset; one installed after the boot is not started
Status statusOf(const Entry& entry) {
    for (const auto& out : status) {
        if ((out.state != State::None) && (out.offset == entry.offset) && (out.crc32 == entry.crc32)) {
            return out;
        }
    }

    return Status{};
}

// --- the guard: modules off after a heap shortage or crashes with them running (setting `modulesOff` = reason)

// survives a crash restart, not a power cut (.noinit is not cleared at boot)
struct Guard {
    uint32_t magic;
    uint32_t running;   // module code started in this boot
    uint32_t booted;    // ... and ran MODULES_BOOT_SECONDS: a later crash is not a boot crash
    uint32_t crashes;
};

constexpr uint32_t GuardMagic { 0x32474D45 }; // "EMG2"

__attribute__((section(".noinit")))
Guard guard;

bool readDirectory(Directory&);
bool writeDirectory(const Directory&);

// the guard's decision is in the directory sector (written at once, outside the settings' rotation)
String offReason() {
    Directory dir;
    if (!readDirectory(dir)) {
        return String();
    }

    dir.off[sizeof(dir.off) - 1] = '\0';
    return String(dir.off);
}

void modulesOff(const String& reason) {
    Directory dir;
    if (!readDirectory(dir)) {
        return;
    }

    std::strncpy(dir.off, reason.c_str(), sizeof(dir.off) - 1);
    dir.off[sizeof(dir.off) - 1] = '\0';
    writeDirectory(dir);
    DEBUG_MSG_P(PSTR("[MODULES] Off: %s\n"), reason.c_str());
}

void modulesOffClear() {
    Directory dir;
    if (readDirectory(dir) && dir.off[0]) {
        dir.off[0] = '\0';
        writeDirectory(dir);
    }
}

bool crashRestart() {
    const auto* info = ESP.getResetInfoPtr();
    return info && ((info->reason == REASON_WDT_RST)
        || (info->reason == REASON_EXCEPTION_RST)
        || (info->reason == REASON_SOFT_WDT_RST));
}

void guardBoot() {
    if (guard.magic != GuardMagic) {
        guard = Guard{GuardMagic, 0, 0, 0};
    }

    // a boot crash: the modules ran, but not past their first MODULES_BOOT_SECONDS. Boots without module code (the
    // unstable mode after the crashes, its exit restart) keep the count, so the crash loop ends here and not in
    // ESPurna's unstable mode every 10 minutes; modules running past their boot clear it (guardLoop)
    if (crashRestart() && guard.running && !guard.booted) {
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

// MODULES.TEST.LOWHEAP: the guard sees no free heap (PROTOTYPE test of the guard)
bool test_low_heap { false };

void guardLoop() {
    static uint32_t last { 0 };
    static uint32_t low { 0 };
    if (millis() - last < 1000) {
        return;
    }
    last = millis();

    if (!guard.booted && (millis() >= (MODULES_BOOT_SECONDS * 1000ul))) {
        guard.booted = 1;
        guard.crashes = 0;
    }

    const auto free = test_low_heap ? 0 : systemFreeHeap();
    if (free >= MODULES_HEAP_MIN) {
        low = 0;
        return;
    }

    if (++low >= MODULES_HEAP_SECONDS) {
        modulesOff(String(F("free heap ")) + String(free, 10) + F(" bytes for ") + String(low, 10)
            + F(" s with the modules running"));
        prepareReset(CustomResetReason::Stability);
    }
}
bool running { false };     // code of a module runs (data modules do not count)

// data of a started data module, e.g. the WebUI
struct Data {
    char name[16];
    const uint8_t* data;
    size_t size;
};

Data data[DirectoryEntries];
size_t data_count { 0 };

// the sector under the settings: EEPROM_Rotate keeps them in its base sector and the pool under it (251 and 250 on a
// 1 MB chip, 8 sectors on 4 MB); the directory at 0xFA000 was in that pool and the settings writes erased it
uint32_t directoryOffset() {
    static uint32_t offset { 0 };
    if (!offset) {
        auto& eeprom = eepromInstance();
        offset = (eeprom.base() - eeprom.size()) * SectorSize;
    }

    return offset;
}

// RAM of a module in IRAM (per module, chosen at the install): the IRAM after the head's code, with the 48 KB IRAM
// layout (flash cache 16 KB). 32-bit access only in hardware: the core's non32xfer handler emulates byte and half
// word access (slow, and not from an interrupt). ram_offset of such an entry has RamInIram set
constexpr uint32_t RamInIram { 0x80000000 };

extern "C" void _text_end(void);

uint32_t iramDataStart() {
    return (reinterpret_cast<uint32_t>(&_text_end) + 32 + 3) & ~3u;
}

uint32_t iramDataSize() {
#if defined(MMU_IRAM_SIZE) && (MMU_IRAM_SIZE > 0x8000) && defined(NON32XFER_HANDLER)
    const uint32_t end = 0x40100000 + MMU_IRAM_SIZE;
    return (end > iramDataStart()) ? (end - iramDataStart()) : 0;
#else
    return 0;
#endif
}

uint32_t headEnd() {
    return (ESP.getSketchSize() + SectorSize - 1) & ~(SectorSize - 1);
}

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

bool readDirectory(Directory& out) {
    if (!ESP.flashRead(directoryOffset(), reinterpret_cast<uint32_t*>(&out), sizeof(out))) {
        return false;
    }

    return (out.magic == DirectoryMagic) && (out.count <= DirectoryEntries);
}

bool writeDirectory(const Directory& dir) {
    if (!ESP.flashEraseSector(directoryOffset() / SectorSize)) {
        return false;
    }

    return ESP.flashWrite(directoryOffset(), reinterpret_cast<const uint32_t*>(&dir), sizeof(dir));
}

bool eraseDirectory() {
    return ESP.flashEraseSector(directoryOffset() / SectorSize);
}

// MD5 of the running sketch, as the module link took it from the head .bin
bool headMd5(uint8_t out[16]) {
    static uint8_t cached[16];
    static bool done { false };
    if (!done) {
        const auto hex = ESP.getSketchMD5();
        if (hex.length() != 32) {
            return false;
        }
        for (size_t index = 0; index < 16; ++index) {
            char pair[3] { hex[index * 2], hex[index * 2 + 1], '\0' };
            cached[index] = static_cast<uint8_t>(strtoul(pair, nullptr, 16));
        }
        done = true;
    }

    std::memcpy(out, cached, 16);
    return true;
}

bool sameHead(const uint8_t md5[16]) {
    uint8_t own[16];
    return headMd5(own) && (std::memcmp(own, md5, 16) == 0);
}

bool flashCrc(uint32_t offset, uint32_t size, uint32_t& out) {
    alignas(4) uint8_t buffer[256];
    uint32_t crc = 0;
    while (size) {
        const uint32_t part = std::min<uint32_t>(size, sizeof(buffer));
        if (!ESP.flashRead(offset, reinterpret_cast<uint32_t*>(buffer), (part + 3) & ~3u)) {
            return false;
        }
        crc = crc32Update(crc, buffer, part);
        offset += part;
        size -= part;
    }

    out = crc;
    return true;
}

bool inside(uint32_t start, uint32_t end, uint32_t lo, uint32_t hi) {
    return (start <= end) && (start >= lo) && (end <= hi);
}

const char* start(const Entry& entry) {
    Header header;
    if (!ESP.flashRead(entry.offset, reinterpret_cast<uint32_t*>(&header), sizeof(header))) {
        return PSTR("header read");
    }

    if ((header.magic != HeaderMagic) || (header.version != HeaderVersion)) {
        return PSTR("no module header");
    }

    if (!sameHead(header.head_md5)) {
        return PSTR("stale");
    }

    uint32_t crc;
    if (!flashCrc(entry.offset, entry.size, crc) || (crc != entry.crc32)) {
        return PSTR("crc");
    }

    const uint32_t flash_lo = FlashMapped + entry.offset;
    const uint32_t flash_hi = flash_lo + entry.size;

    // data module: only its bytes, nothing to copy or call
    if (!header.entry) {
        if ((header.flash_start != flash_lo) || (header.flash_end != flash_hi)
         || (header.data_end > (entry.size - sizeof(Header))))
        {
            return PSTR("ranges");
        }

        return nullptr;
    }

    const bool in_iram = (entry.ram_offset & RamInIram) != 0;
    const uint32_t ram_lo = in_iram ? iramDataStart() : reinterpret_cast<uint32_t>(&espurna_modules_ram_arena[0]);
    const uint32_t ram_hi = ram_lo + (in_iram ? iramDataSize() : sizeof(espurna_modules_ram_arena));
    const uint32_t iram_lo = reinterpret_cast<uint32_t>(&espurna_modules_iram_arena[0]);
    const uint32_t iram_hi = iram_lo + sizeof(espurna_modules_iram_arena);

    if ((header.flash_start != flash_lo) || !inside(header.flash_start, header.flash_end, flash_lo, flash_hi)
     || !inside(header.data_start, header.data_end, ram_lo, ram_hi)
     || !inside(header.bss_start, header.bss_end, ram_lo, ram_hi)
     || !inside(header.iram_start, header.iram_end, iram_lo, iram_hi)
     || !inside(header.data_load, header.data_load + (header.data_end - header.data_start), flash_lo, flash_hi)
     || !inside(header.iram_load, header.iram_load + (header.iram_end - header.iram_start), flash_lo, flash_hi)
     || !inside(header.init_start, header.init_end, flash_lo, flash_hi)
     || !inside(header.entry, header.entry + 4, flash_lo, flash_hi))
    {
        return PSTR("ranges");
    }

    // .data + .rodata, then .bss; in IRAM with 32 bit stores only
    const uint32_t data_size = header.data_end - header.data_start;
    if (in_iram) {
        const auto* src = reinterpret_cast<const volatile uint32_t*>(header.data_load);
        auto* dst = reinterpret_cast<volatile uint32_t*>(header.data_start);
        for (uint32_t index = 0; index < data_size / 4; ++index) {
            dst[index] = src[index];
        }
        auto* bss = reinterpret_cast<volatile uint32_t*>(header.bss_start);
        for (uint32_t index = 0; index < (header.bss_end - header.bss_start) / 4; ++index) {
            bss[index] = 0;
        }
    } else {
        if (data_size && !ESP.flashRead(header.data_load - FlashMapped,
                reinterpret_cast<uint32_t*>(header.data_start), data_size))
        {
            return PSTR("data read");
        }
        std::memset(reinterpret_cast<void*>(header.bss_start), 0, header.bss_end - header.bss_start);
    }

    // IRAM takes 32 bit stores only, words from the mapped flash
    {
        const auto* src = reinterpret_cast<const volatile uint32_t*>(header.iram_load);
        auto* dst = reinterpret_cast<volatile uint32_t*>(header.iram_start);
        for (uint32_t index = 0; index < (header.iram_end - header.iram_start) / 4; ++index) {
            dst[index] = src[index];
        }
    }

    using Func = void(*)();
    for (auto* ptr = reinterpret_cast<const Func*>(header.init_start);
            ptr != reinterpret_cast<const Func*>(header.init_end); ++ptr)
    {
        (*ptr)();
    }

    reinterpret_cast<Func>(header.entry)();
    return nullptr;
}

// --- upload --------------------------------------------------------------------------------------

struct Upload {
    alignas(4) uint8_t buffer[512];
    uint8_t map[MapSize];
    PackageHeader header;

    size_t received { 0 };      // bytes of the request body
    size_t body { 0 };          // bytes after the package header
    uint32_t body_crc { 0 };
    uint32_t image_crc { 0 };

    uint32_t slot { 0 };        // flash offset
    bool ram_in_iram { false }; // ?ram=iram
    bool replace { false };     // ?replace=1: a new copy of an installed module (its RAM moved), swapped in when written
    uint32_t ram_offset { 0 };
    uint32_t iram_offset { 0 };
    uint32_t delta[4] {};

    uint32_t written { 0 };     // image bytes placed
    size_t buffered { 0 };
    uint32_t word { 0 };
    size_t word_size { 0 };
    size_t map_fill { 0 };
    size_t chunk_left { 0 };    // image bytes of the current chunk still to come

    const char* error { nullptr };
    bool done { false };
};

Upload* upload { nullptr };
AsyncWebServerRequest* owner { nullptr };

// flash of the modules started in this boot: a module switched off keeps running from it until the restart, so a
// module switched on before the restart (the same one with its RAM moved, too) must be placed elsewhere
struct BootPiece {
    uint32_t offset;
    uint32_t size;
};

BootPiece boot_flash[DirectoryEntries];
size_t boot_flash_count { 0 };

// the space of one kind (flash between the head and the directory, an arena) with the modules' pieces in it; flash
// has the directory's pieces and the pieces still running from this boot
struct Space {
    uint32_t lo;
    uint32_t hi;
    uint32_t start[DirectoryEntries * 2];
    uint32_t end[DirectoryEntries * 2];
    size_t count { 0 };

    void add(uint32_t from, uint32_t size) {
        if (!size) {
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

    // the gap before piece `index` (count: the one after the last)
    uint32_t gapStart(size_t index) const {
        return index ? std::max(lo, end[index - 1]) : lo;
    }

    uint32_t gapEnd(size_t index) const {
        return (index < count) ? std::min(hi, start[index]) : hi;
    }

    uint32_t gap(size_t index) const {
        const auto a = gapStart(index);
        const auto b = gapEnd(index);
        return (b > a) ? (b - a) : 0;
    }

    uint32_t free() const {
        uint32_t out = 0;
        for (size_t index = 0; index <= count; ++index) {
            out += gap(index);
        }
        return out;
    }

    uint32_t largest() const {
        uint32_t out = 0;
        for (size_t index = 0; index <= count; ++index) {
            out = std::max(out, gap(index));
        }
        return out;
    }

    // best fit: the smallest gap that holds it (a removed module's hole goes to one of its size, the big gap stays
    // for a big one); flash at the top of the gap (the head grows from below), the arenas at the bottom
    bool place(uint32_t size, bool top, uint32_t& out) const {
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
};

struct Spaces {
    Space flash;
    Space ram;
    Space iram;
    Space iram_data;
};

// `arenas_of_other`: that module's RAM and IRAM count as free (a new copy of it replaces it at the next boot)
void spaces(const Directory& dir, bool valid, Spaces& out, const char* arenas_of_other = nullptr) {
    out.flash.lo = headEnd();
    out.flash.hi = directoryOffset();
    out.ram.lo = 0;
    out.ram.hi = sizeof(espurna_modules_ram_arena);
    out.iram.lo = 0;
    out.iram.hi = sizeof(espurna_modules_iram_arena);
    out.iram_data.lo = 0;
    out.iram_data.hi = iramDataSize();
    for (size_t index = 0; valid && (index < dir.count); ++index) {
        const auto& entry = dir.entries[index];
        out.flash.add(entry.offset, (entry.size + SectorSize - 1) & ~(SectorSize - 1));
        if (arenas_of_other && (std::strncmp(entry.name, arenas_of_other, sizeof(entry.name)) == 0)) {
            continue;
        }
        if (entry.ram_offset & RamInIram) {
            out.iram_data.add(entry.ram_offset & ~RamInIram, entry.ram_size);
        } else {
            out.ram.add(entry.ram_offset, entry.ram_size);
        }
        out.iram.add(entry.iram_offset, entry.iram_size);
    }

    // switched off, still running: its flash stays taken until the restart (RAM is taken only at the next boot)
    for (size_t piece = 0; piece < boot_flash_count; ++piece) {
        bool listed = false;
        for (size_t index = 0; valid && (index < dir.count); ++index) {
            listed = listed || (dir.entries[index].offset == boot_flash[piece].offset);
        }
        if (!listed) {
            out.flash.add(boot_flash[piece].offset, boot_flash[piece].size);
        }
    }
}

// --- the modules file in flash: 4 MB boards flashed with the full image carry it at 1 MB, behind the 1 MB layout
// the head runs in. The SDK refuses reads past the size in the image header (1 MB): the size is raised for the read

constexpr uint32_t OnboardFile { 0x100000 };
constexpr uint32_t ContainerMagic { 0x31434D45 }; // "EMC1"
constexpr size_t ContainerEntry { 48 };

bool readPastHeader(uint32_t address, uint32_t* data, size_t size) {
    const uint32_t real = ESP.getFlashChipRealSize();
    if ((address + size) > real) {
        return false;
    }

    const uint32_t saved = flashchip->chip_size;
    flashchip->chip_size = real;
    const bool ok = (spi_flash_read(address, data, size) == SPI_FLASH_RESULT_OK);
    flashchip->chip_size = saved;
    return ok;
}

// bytes of the modules file at 1 MB, 0 when there is none
uint32_t onboardFileSize() {
    static uint32_t size { 0xffffffff };
    if (size != 0xffffffff) {
        return size;
    }

    size = 0;
    if (ESP.getFlashChipRealSize() <= OnboardFile) {
        return size;
    }

    uint32_t head[2];
    if (!readPastHeader(OnboardFile, head, sizeof(head)) || (head[0] != ContainerMagic) || !head[1]
        || (head[1] > DirectoryEntries))
    {
        return size;
    }

    uint32_t end = 8 + (head[1] * ContainerEntry);
    for (uint32_t index = 0; index < head[1]; ++index) {
        uint32_t entry[12];
        if (!readPastHeader(OnboardFile + 8 + (index * ContainerEntry), entry, sizeof(entry))) {
            return size;
        }
        // name[16], offset, size
        end = std::max(end, entry[4] + entry[5]);
    }

    if ((OnboardFile + end) <= ESP.getFlashChipRealSize()) {
        size = end;
    }

    return size;
}

void onFile(AsyncWebServerRequest* request) {
    if (!webAuthenticate(request)) {
        return request->requestAuthentication(systemHostname().c_str());
    }

    const uint32_t size = onboardFileSize();
    if (!size) {
        request->send(404, String(F("text/plain")), String(F("ERROR: no modules file in flash")));
        return;
    }

    auto* response = request->beginResponse(String(F("application/octet-stream")), size,
        [size](uint8_t* buffer, size_t max, size_t index) -> size_t {
            if (index >= size) {
                return 0;
            }

            // aligned reads of at most 256 bytes, copied out at the asked offset
            alignas(4) uint8_t part[260];
            const uint32_t address = OnboardFile + index;
            const uint32_t aligned = address & ~3u;
            const size_t skip = address - aligned;
            const size_t want = std::min<size_t>(std::min<size_t>(max, size - index), sizeof(part) - 4);
            if (!readPastHeader(aligned, reinterpret_cast<uint32_t*>(part), (skip + want + 3) & ~3u)) {
                return 0;
            }

            std::memcpy(buffer, part + skip, want);
            return want;
        });
    response->addHeader(String(F("Cache-Control")), String(F("no-store")));
    request->send(response);
}

const char* prepare(Upload& up) {
    const auto& header = up.header;
    if ((header.magic != PackageMagic) || (header.version != 1)) {
        return PSTR("not a module package");
    }

    if (!sameHead(header.head_md5)) {
        return PSTR("the module was built for another head image");
    }

    if (!header.image_size || (header.image_size % 4)) {
        return PSTR("bad image size");
    }

    Directory dir;
    if (!readDirectory(dir)) {
        std::memset(&dir, 0, sizeof(dir));
    }

    bool installed = false;
    for (size_t index = 0; index < dir.count; ++index) {
        if (std::strncmp(dir.entries[index].name, header.name, sizeof(dir.entries[index].name)) == 0) {
            if (!up.replace) {
                return PSTR("installed already (MODULES.REMOVE first)");
            }
            installed = true;
        }
    }

    if (!installed && (dir.count >= DirectoryEntries)) {
        return PSTR("directory full");
    }

    // a new copy of an installed module: the arenas are filled at boot, so the old copy's RAM and IRAM are free for
    // it; its flash is not (running code, or the copy that stays when this upload fails)
    Spaces space;
    spaces(dir, true, space, installed ? header.name : nullptr);

    const uint32_t size = (header.image_size + SectorSize - 1) & ~(SectorSize - 1);
    if (!space.flash.place(size, true, up.slot)) {
        return PSTR("not enough flash");
    }

    const uint32_t ram_size = (header.ram_size + 3) & ~3u;
    if (ram_size) {
        auto& target = up.ram_in_iram ? space.iram_data : space.ram;
        if (!target.place(ram_size, false, up.ram_offset)) {
            return up.ram_in_iram ? PSTR("not enough IRAM for its RAM") : PSTR("not enough RAM in the arena");
        }
    } else {
        up.ram_offset = 0;
        up.ram_in_iram = false;
    }

    const uint32_t iram_size = (header.iram_size + 3) & ~3u;
    if (iram_size) {
        if (!space.iram.place(iram_size, false, up.iram_offset)) {
            return PSTR("not enough IRAM in the arena");
        }
    } else {
        up.iram_offset = 0;
    }

    const uint32_t ram_end = up.ram_offset;
    const uint32_t iram_end = up.iram_offset;
    up.delta[0] = 0;
    up.delta[1] = (FlashMapped + up.slot) - header.link_flash;
    const uint32_t ram_base = up.ram_in_iram ? iramDataStart() : reinterpret_cast<uint32_t>(&espurna_modules_ram_arena[0]);
    up.delta[2] = (ram_base + ram_end) - header.link_ram;
    up.delta[3] = (reinterpret_cast<uint32_t>(&espurna_modules_iram_arena[0]) + iram_end) - header.link_iram;

    return nullptr;
}

bool flush(Upload& up) {
    if (!up.buffered) {
        return true;
    }

    const uint32_t address = up.slot + up.written;
    if ((address % SectorSize) == 0) {
        if (!ESP.flashEraseSector(address / SectorSize)) {
            return false;
        }
    }

    // chunks are 4 KiB and sector aligned, the buffer divides a sector: a flush never crosses a sector
    if (!ESP.flashWrite(address, reinterpret_cast<const uint32_t*>(up.buffer), up.buffered)) {
        return false;
    }

    up.image_crc = crc32Update(up.image_crc, up.buffer, up.buffered);
    up.written += up.buffered;
    up.buffered = 0;
    return true;
}

// one image word, in chunk order
bool place(Upload& up, uint32_t value) {
    const size_t index = ((up.written + up.buffered) % ChunkSize) / 4;
    const uint8_t kind = (up.map[index >> 2] >> ((index & 3) * 2)) & 3;
    value += up.delta[kind];

    std::memcpy(&up.buffer[up.buffered], &value, 4);
    up.buffered += 4;
    if (up.buffered == sizeof(up.buffer)) {
        return flush(up);
    }

    return true;
}

void feed(Upload& up, const uint8_t* data, size_t len) {
    while (len && !up.error) {
        // package header
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

        if (up.written + up.buffered >= up.header.image_size) {
            up.error = PSTR("more data than the image");
            return;
        }

        // map of the next chunk
        if (!up.chunk_left) {
            const size_t part = std::min(len, MapSize - up.map_fill);
            std::memcpy(&up.map[up.map_fill], data, part);
            up.body_crc = crc32Update(up.body_crc, data, part);
            up.map_fill += part;
            up.received += part;
            data += part;
            len -= part;
            if (up.map_fill == MapSize) {
                up.map_fill = 0;
                up.chunk_left = std::min<size_t>(ChunkSize, up.header.image_size - (up.written + up.buffered));
            }
            continue;
        }

        // chunk bytes, word by word
        const size_t part = std::min(len, up.chunk_left);
        up.body_crc = crc32Update(up.body_crc, data, part);
        for (size_t index = 0; index < part; ++index) {
            up.word |= static_cast<uint32_t>(data[index]) << (8 * up.word_size);
            if (++up.word_size == 4) {
                if (!place(up, up.word)) {
                    up.error = PSTR("flash write");
                    return;
                }
                up.word = 0;
                up.word_size = 0;
            }
        }
        up.chunk_left -= part;
        up.received += part;
        data += part;
        len -= part;
    }
}

const char* finish(Upload& up) {
    if (up.error) {
        return up.error;
    }

    if (up.received < sizeof(PackageHeader)) {
        return PSTR("short upload");
    }

    if (!flush(up)) {
        return PSTR("flash write");
    }

    if (up.written != up.header.image_size) {
        return PSTR("short upload");
    }

    if (up.body_crc != up.header.crc32) {
        return PSTR("package crc");
    }

    uint32_t crc;
    if (!flashCrc(up.slot, up.written, crc) || (crc != up.image_crc)) {
        return PSTR("verify");
    }

    Directory dir;
    if (!readDirectory(dir)) {
        std::memset(&dir, 0, sizeof(dir));
        dir.magic = DirectoryMagic;
    }

    size_t index = dir.count;
    for (size_t other = 0; up.replace && (other < dir.count); ++other) {
        if (std::strncmp(dir.entries[other].name, up.header.name, sizeof(dir.entries[other].name)) == 0) {
            index = other;
        }
    }

    if (index >= DirectoryEntries) {
        return PSTR("directory full");
    }

    auto& entry = dir.entries[index];
    std::memset(&entry, 0, sizeof(entry));
    std::memcpy(entry.name, up.header.name, sizeof(entry.name));
    entry.name[sizeof(entry.name) - 1] = '\0';
    entry.offset = up.slot;
    entry.size = up.written;
    entry.crc32 = crc;
    entry.ram_offset = up.ram_offset | (up.ram_in_iram ? RamInIram : 0);
    entry.ram_size = (up.header.ram_size + 3) & ~3u;
    entry.iram_offset = up.iram_offset;
    entry.iram_size = (up.header.iram_size + 3) & ~3u;
    if (index == dir.count) {
        ++dir.count;
    }

    if (!writeDirectory(dir)) {
        return PSTR("directory write");
    }

    return nullptr;
}

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

void onUploadDone(AsyncWebServerRequest* request) {
    if (!webAuthenticate(request)) {
        return request->requestAuthentication(systemHostname().c_str());
    }

    if (!upload || (owner != request)) {
        answer(request, 400, F("ERROR: no module upload in this request"));
        return;
    }

    const auto* error = finish(*upload);
    String text;
    if (error) {
        text = String(F("ERROR: ")) + FPSTR(error);
        answer(request, 400, text);
    } else {
        text = String(F("OK, ")) + upload->header.name + F(" at 0x") + String(upload->slot, 16)
            + F(", restart to start it");
        answer(request, 200, text);
    }

    DEBUG_MSG_P(PSTR("[MODULES] Upload: %s\n"), text.c_str());
    release();
}

void onUploadData(AsyncWebServerRequest* request, String, size_t index, uint8_t* data, size_t len, bool) {
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

        if (otaWebUploading() || Update.isRunning()) {
            return;
        }

        upload = new (std::nothrow) Upload();
        if (!upload) {
            return;
        }

        const auto* ram = request->getParam(String(F("ram")));
        upload->ram_in_iram = ram && ram->value().equals(F("iram"));
        upload->replace = request->hasParam(String(F("replace")));

        owner = request;
        request->onDisconnect([request]() {
            if (owner == request) {
                release();
            }
        });
    }

    if (upload && (owner == request)) {
        feed(*upload, data, len);
    }
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
    case State::Broken:
        break;
    }

    return F("broken");
}

// --- the modules page ------------------------------------------------------------------------------

// /modules.html, and / while there is no WebUI module: a switch per module, with what it takes of the flash, the RAM
// and the IRAM (TCM) arenas, and what is taken already. The modules come from the modules file of the running release
// (espurna-esp8266-1m-modules.bin, from the disk or from the release list); on: its package is uploaded to
// /modules/upload, off: /modules/remove. Both apply at the next restart
alignas(4) static constexpr char ModulesPage[] PROGMEM = R"~(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>ESPurna modules</title>
<style>body{font:15px system-ui,sans-serif;margin:0 auto;padding:16px;max-width:760px;color:#222;background:#fff}
h2{margin:.2em 0}.sub{color:#666;font-size:13px;margin-bottom:12px}
.bar{display:flex;height:22px;border-radius:4px;overflow:hidden;background:#e6e6e6;margin:4px 0 2px}
.bar i{display:block;height:100%}.h{background:#8090a8}.m{background:#2f80c8}.n{background:#a6d8a6}
.y{background:#f0b429}.tight{color:#b07800}
.lg{font-size:13px;color:#444;margin-bottom:12px}
table{border-collapse:collapse;width:100%}td,th{padding:6px 4px;border-bottom:1px solid #eee;text-align:left;font-size:14px}
.r{text-align:right}.st{font-size:12px;color:#666}.bad{color:#c00}
button{font-size:14px;padding:6px 12px;margin:4px 4px 4px 0}input[type=checkbox]{width:20px;height:20px}
#msg{white-space:pre-wrap;background:#f4f4f4;padding:8px;min-height:1em;font-size:13px}</style>
<h2>Modules</h2><div class=sub id=ver></div>
<div id=off style="display:none;background:#fde2e1;border:1px solid #d9534f;padding:8px;margin-bottom:12px">
<b>Modules are off</b>: <span id=why></span><br><button id=on>Start the modules again</button></div><div id=bars></div>
<table><thead><tr><th></th><th>Module</th><th class=r>Flash</th><th class=r>RAM</th><th class=r>IRAM</th>
<th>RAM in</th><th>State</th></tr></thead><tbody id=list></tbody></table>
<p>Modules file or full image: <input type=file id=file accept=.bin> <button id=gh>From GitHub</button></p>
<p><button id=rs>Restart</button> <a href="/">WebUI</a></p><div id=msg></div>
<script>
var S,A={},W={},$=function(i){return document.getElementById(i)};
function kb(v){return (v/1024).toFixed(1)+' KB'}
function sec(v){return Math.ceil(v/4096)*4096}
function msg(t){$('msg').textContent=t}
var TIGHT=0.85;
function bar(t,total,parts,lg){var taken=0,h,y;parts.forEach(function(p){if(p[0]!='n')taken+=p[1]});y=taken>=TIGHT*total;
h='<b>'+t+'</b>'+(y?' <span class=tight>almost full</span>':'')+'<div class=bar>';parts.forEach(function(p){if(p[1]>0)
h+='<i class='+(y&&p[0]=='m'?'y':p[0])+' title="'+p[2]+'" style="width:'+(100*p[1]/total)+'%"></i>'});
return h+'</div><div class=lg>'+lg+'</div>'}
function load(){return fetch('/modules',{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){S=j;draw();
if(S.file&&!Object.keys(A).length)return fetch('/modules/file').then(function(r){if(!r.ok)throw r.status;return r.arrayBuffer()})
.then(function(b){parse(b);msg('modules file: from this board ('+Object.keys(A).join(', ')+')')})})}
function find(n){return S.modules.filter(function(x){return x.name==n})[0]}
function draw(){var used=0,rf=S.ram_arena-S.ram_used,irf=S.iram_arena-S.iram_used,names={},h='';
S.modules.forEach(function(m){used+=sec(m.size);names[m.name]=1});Object.keys(A).forEach(function(n){names[n]=1});
$('ver').textContent=S.version+', '+S.image;$('off').style.display=S.off?'block':'none';$('why').textContent=S.off;
$('bars').innerHTML=bar('Flash',S.area,[['h',S.head,'head'],['m',used,'modules'],['n',S.free,'free']],
'head '+kb(S.head)+' + modules '+kb(used)+' of '+kb(S.area)+', free '+kb(S.free))+
bar('RAM',S.ram_arena,[['m',S.ram_used,'modules'],['n',rf,'free']],'modules '+S.ram_used+' of '+S.ram_arena+
' bytes, free '+rf+'; heap free '+kb(S.heap)+' (after boot '+kb(S.heap_boot)+')')+
bar('IRAM (TCM)',S.iram_arena,[['m',S.iram_used,'modules'],['n',irf,'free']],'module code '+S.iram_used+' of '+
S.iram_arena+' bytes, free '+irf)+(S.iram_data?bar('RAM in IRAM',S.iram_data,[['m',S.iram_data_used,'modules'],
['n',S.iram_data-S.iram_data_used,'free']],'module RAM '+S.iram_data_used+' of '+S.iram_data+' bytes, free '+
(S.iram_data-S.iram_data_used)+' (32-bit memory: byte access is emulated, slower)'):'');
Object.keys(names).sort().forEach(function(n){var m=find(n),a=A[n],fit=!!m,tight=false;
var why=[],ii=W[n]=='iram';if(!m){if(sec(a.image)>S.free_max)why.push('flash');
if(ii?a.ram>(S.iram_data_max||0):a.ram>S.ram_max)why.push(ii?'IRAM for its RAM':'RAM');
if(a.iram>S.iram_max)why.push('IRAM');fit=!why.length;
tight=fit&&((S.ram_used+a.ram)>=TIGHT*S.ram_arena||(S.iram_used+a.iram)>=TIGHT*S.iram_arena||
(S.area-S.free+sec(a.image))>=TIGHT*S.area)}
h+='<tr><td><input type=checkbox data-n="'+n+'"'+(m?' checked':'')+(fit?'':' disabled')+'></td><td>'+n+
'</td><td class=r>'+kb(m?m.size:a.image)+'</td><td class=r>'+(m?m.ram:a.ram)+'</td><td class=r>'+(m?m.iram:a.iram)+
'</td><td>'+cell(n,m,a)+
'</td><td class="st'+(fit?(tight?' tight':''):' bad')+'">'+(m?m.state:(fit?(tight?'fits, tight':'can be added'):
'no '+why.join(', ')+' for it'))+'</td></tr>'});
$('list').innerHTML=h;Array.prototype.forEach.call(document.querySelectorAll('input[data-n]'),function(e){
e.onchange=function(){toggle(e)}});Array.prototype.forEach.call(document.querySelectorAll('select[data-r]'),function(e){
e.onchange=function(){var n=e.getAttribute('data-r');if(e.getAttribute('data-m'))move(n,e.value);else{W[n]=e.value;draw()}}})}
function canMove(m,a){var to=m.ram_in=='iram'?'dram':'iram';return (m.state=='running'||m.state=='not started')&&
sec(a.image)<=S.free_max&&(to=='iram'?a.ram<=(S.iram_data_max||0):a.ram<=S.ram_max)}
function cell(n,m,a){var w=m?(m.ram_in||'dram'):(W[n]=='iram'?'iram':'dram');if(!S.iram_data||!a||!a.ram||(m&&!canMove(m,a)))
return w.toUpperCase();return '<select data-r="'+n+'"'+(m?' data-m=1':'')+'><option value=dram'+(w=='dram'?' selected':'')+
'>DRAM<option value=iram'+(w=='iram'?' selected':'')+'>IRAM</select>'}
function move(n,v){var u=new FormData();u.append('module',new Blob([A[n].data]),n+'.module');
msg(n+': moving its RAM to '+v.toUpperCase()+'...');post('/modules/upload?replace=1'+(v=='iram'?'&ram=iram':''),u).then(function(t){
msg(n+': '+t);$('rs').style.background='#f0ad4e'},function(x){msg(n+': '+x)}).then(load)}
function post(u,d){return fetch(u,{method:'POST',body:d}).then(function(r){return r.text().then(function(t){
if(!r.ok)throw t||r.status;return t})})}
function toggle(e){var n=e.getAttribute('data-n'),d=new FormData();e.disabled=true;
if(e.checked){if(!A[n]){msg(n+': not in the modules file');return load()}
d.append('module',new Blob([A[n].data]),n+'.module');msg(n+': uploading...')}else{d.append('name',n);msg(n+': removing...')}
post(e.checked?'/modules/upload'+(W[n]=='iram'?'?ram=iram':''):'/modules/remove',d).then(function(t){msg(n+': '+t);$('rs').style.background='#f0ad4e'},
function(x){msg(n+': '+x)}).then(load)}
function parse(b){var v=new DataView(b),o={},c,i,k,j,nm,ch,z=0;if(b.byteLength>0x100008&&v.getUint8(0)==0xE9&&
v.getUint32(0x100000,true)==0x31434d45)z=0x100000;if(b.byteLength<z+8||v.getUint32(z,true)!=0x31434d45)
throw 'not a modules file (or the full image)';c=v.getUint32(z+4,true);for(i=0;i<c;i++){k=z+8+48*i;nm='';
for(j=0;j<16;j++){ch=v.getUint8(k+j);if(!ch)break;nm+=String.fromCharCode(ch)}o[nm]={image:v.getUint32(k+24,true),
ram:v.getUint32(k+28,true),iram:v.getUint32(k+32,true),data:new Uint8Array(b,z+v.getUint32(k+16,true),
v.getUint32(k+20,true))}}
A=o;msg('modules file: '+Object.keys(o).join(', '));draw()}
$('file').onchange=function(){var f=this.files[0];if(f)f.arrayBuffer().then(parse).catch(function(x){msg(''+x)})};
$('gh').onclick=function(){var rev=(S.version.split('-git')[1]||'').slice(0,7);msg('looking for the modules of '+rev+'...');
fetch(S.list||'https://igorufox.github.io/espurna/firmware.json').then(function(r){return r.json()}).then(function(j){
var u=null;(j.releases||[]).forEach(function(r){if(String(r.commit||'').indexOf(rev)==0)((r.images||{})[S.image]||[])
.forEach(function(f){if(f.kind=='modules'||(f.kind=='full'&&!u))u=f.url})});if(!u)throw 'no modules file of '+rev+
' in the release list, pick the file from the disk';return fetch(u).then(function(r){return r.arrayBuffer()}).then(parse)})
.catch(function(x){msg(''+x)})};
$('on').onclick=function(){post('/modules/on',new FormData()).then(function(t){msg(t);
setTimeout(function(){location.reload()},15000)})};
$('rs').onclick=function(){post('/modules/restart',new FormData()).then(function(){msg('restarting...');
setTimeout(function(){location.reload()},15000)})};
load().catch(function(x){msg(''+x)});
</script>
)~";

void onModulesPage(AsyncWebServerRequest* request) {
    if (!webAuthenticate(request)) {
        return request->requestAuthentication(systemHostname().c_str());
    }

    auto* response = request->beginResponse_P(200, String(F("text/html")),
        reinterpret_cast<const uint8_t*>(ModulesPage), sizeof(ModulesPage) - 1);
    request->send(response);
}

void jsonNumber(String& out, const __FlashStringHelper* name, uint32_t value) {
    out += ",\"";
    out += name;
    out += "\":";
    out += String(value, 10);
}

void onStatus(AsyncWebServerRequest* request) {
    if (!webAuthenticate(request)) {
        return request->requestAuthentication(systemHostname().c_str());
    }

    Directory dir;
    const bool valid = readDirectory(dir);
    Spaces space;
    spaces(dir, valid, space);

    String out;
    out.reserve(1024);
    out += F("{\"version\":\"");
    out += buildApp().version.toString();
    out += F("\",\"image\":\"");
    out += F(ESPURNA_IMAGE);
    out += F("\",\"md5\":\"");
    out += ESP.getSketchMD5();
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
        jsonNumber(out, F("size"), entry.size);
        jsonNumber(out, F("offset"), entry.offset);
        jsonNumber(out, F("ram"), entry.ram_size);
        out += (entry.ram_offset & RamInIram) ? F(",\"ram_in\":\"iram\"") : F(",\"ram_in\":\"dram\"");
        jsonNumber(out, F("iram"), entry.iram_size);
        out += F(",\"state\":\"");
        out += stateName(statusOf(entry));
        out += F("\"}");
    }
    out += ']';

    jsonNumber(out, F("area"), directoryOffset());
    jsonNumber(out, F("head"), headEnd());
    jsonNumber(out, F("free"), space.flash.free());
    jsonNumber(out, F("free_max"), space.flash.largest());
    jsonNumber(out, F("ram_arena"), sizeof(espurna_modules_ram_arena));
    jsonNumber(out, F("ram_used"), sizeof(espurna_modules_ram_arena) - space.ram.free());
    jsonNumber(out, F("ram_max"), space.ram.largest());
    jsonNumber(out, F("iram_arena"), sizeof(espurna_modules_iram_arena));
    jsonNumber(out, F("iram_used"), sizeof(espurna_modules_iram_arena) - space.iram.free());
    jsonNumber(out, F("iram_max"), space.iram.largest());
    jsonNumber(out, F("iram_data"), iramDataSize());
    jsonNumber(out, F("iram_data_used"), iramDataSize() - space.iram_data.free());
    jsonNumber(out, F("iram_data_max"), space.iram_data.largest());
    jsonNumber(out, F("heap"), systemFreeHeap());
    jsonNumber(out, F("heap_boot"), systemInitialFreeHeap());
    jsonNumber(out, F("chip"), ESP.getFlashChipRealSize());
    jsonNumber(out, F("file"), onboardFileSize());
    out += '}';

    request->send(200, String(F("application/json")), out);
}

void onModulesOn(AsyncWebServerRequest* request) {
    if (!webAuthenticate(request)) {
        return request->requestAuthentication(systemHostname().c_str());
    }

    modulesOffClear();
    request->send(200, String(F("text/plain")), String(F("OK, modules on, restarting")));
    prepareReset(CustomResetReason::Web);
}

// a module out of the directory; its code keeps running until the restart
void onRemove(AsyncWebServerRequest* request) {
    if (!webAuthenticate(request)) {
        return request->requestAuthentication(systemHostname().c_str());
    }

    const auto* param = request->getParam(String(F("name")), true);
    if (!param) {
        request->send(400, String(F("text/plain")), String(F("ERROR: no name")));
        return;
    }

    Directory dir;
    if (!readDirectory(dir)) {
        request->send(404, String(F("text/plain")), String(F("ERROR: no modules")));
        return;
    }

    size_t out = 0;
    for (size_t index = 0; index < dir.count; ++index) {
        if (!param->value().equals(dir.entries[index].name)) {
            dir.entries[out++] = dir.entries[index];
        }
    }

    if (out == dir.count) {
        request->send(404, String(F("text/plain")), String(F("ERROR: not installed")));
        return;
    }

    dir.count = out;
    if (!writeDirectory(dir)) {
        request->send(500, String(F("text/plain")), String(F("ERROR: directory write")));
        return;
    }

    request->send(200, String(F("text/plain")), String(F("OK, removed, restart to apply")));
}

void onRestart(AsyncWebServerRequest* request) {
    if (!webAuthenticate(request)) {
        return request->requestAuthentication(systemHostname().c_str());
    }

    request->send(200, String(F("text/plain")), String(F("OK")));
    prepareReset(CustomResetReason::Web);
}

// --- terminal ------------------------------------------------------------------------------------

#if TERMINAL_SUPPORT
PROGMEM_STRING(List, "MODULES");
PROGMEM_STRING(Off, "MODULES.OFF");
PROGMEM_STRING(Remove, "MODULES.REMOVE");
PROGMEM_STRING(On, "MODULES.ON");
PROGMEM_STRING(TestLowHeap, "MODULES.TEST.LOWHEAP");
PROGMEM_STRING(TestCrash, "MODULES.TEST.CRASH");

void commandTestLowHeap(::terminal::CommandContext&& ctx) {
    test_low_heap = true;
    terminalOK(ctx);
}

void commandTestCrash(::terminal::CommandContext&& ctx) {
    terminalOK(ctx);
    ctx.output.flush();
    *reinterpret_cast<volatile uint32_t*>(0) = 0;
}

void commandOn(::terminal::CommandContext&& ctx) {
    modulesOffClear();
    terminalOK(ctx);
    prepareReset(CustomResetReason::Terminal);
}


void commandList(::terminal::CommandContext&& ctx) {
    Directory dir;
    const bool valid = readDirectory(dir);

    uint8_t md5[16] {};
    headMd5(md5);
    ctx.output.printf_P(PSTR("loader rev 2, head: %u bytes, md5 %02x%02x%02x%02x..., directory at 0x%05x\n"),
        ESP.getSketchSize(), md5[0], md5[1], md5[2], md5[3], directoryOffset());
    ctx.output.printf_P(PSTR("arenas: RAM %u bytes at %p, IRAM %u bytes at %p\n"),
        sizeof(espurna_modules_ram_arena), &espurna_modules_ram_arena[0],
        sizeof(espurna_modules_iram_arena), &espurna_modules_iram_arena[0]);

    if (valid) {
        for (size_t index = 0; index < dir.count; ++index) {
            const auto& entry = dir.entries[index];
            ctx.output.printf_P(PSTR("%-15s 0x%05x %6u bytes, RAM %u @%u, IRAM %u @%u: %s%s%s\n"),
                entry.name, entry.offset, entry.size, entry.ram_size, entry.ram_offset,
                entry.iram_size, entry.iram_offset,
                String(stateName(statusOf(entry))).c_str(),
                statusOf(entry).reason ? " - " : "",
                statusOf(entry).reason ? String(FPSTR(statusOf(entry).reason)).c_str() : "");
        }
    } else {
        ctx.output.println(F("no modules"));
    }

    Spaces space;
    spaces(dir, valid, space);
    ctx.output.printf_P(PSTR("free for modules: flash %u (largest %u), RAM %u (%u), IRAM %u (%u) bytes\n"),
        space.flash.free(), space.flash.largest(), space.ram.free(), space.ram.largest(),
        space.iram.free(), space.iram.largest());
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

    Directory dir;
    if (!readDirectory(dir)) {
        terminalError(ctx, F("no modules"));
        return;
    }

    size_t out = 0;
    for (size_t index = 0; index < dir.count; ++index) {
        if (!ctx.argv[1].equals(dir.entries[index].name)) {
            dir.entries[out++] = dir.entries[index];
        }
    }

    if (out == dir.count) {
        terminalError(ctx, F("not installed"));
        return;
    }

    dir.count = out;
    if (!writeDirectory(dir)) {
        terminalError(ctx, F("directory write"));
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
};
#endif

} // namespace
} // namespace modules
} // namespace espurna

// before the stability check: the commands are there in the unstable mode as well
void modulesBootSetup() {
#if TERMINAL_SUPPORT
    espurna::terminal::add(espurna::modules::Commands);
#endif

    espurna::modules::guardBoot();

    espurna::modules::Directory dir;
    if (!espurna::modules::readDirectory(dir)) {
        return;
    }

    for (size_t index = 0; index < dir.count; ++index) {
        espurna::modules::status[index] = espurna::modules::Status{
            espurna::modules::State::Skipped, nullptr, dir.entries[index].offset, dir.entries[index].crc32};
    }
}

// stable boot only, where the module's own setup used to be
void modulesSetup() {
    using namespace espurna::modules;

    webServer().on("/modules/upload", HTTP_POST, onUploadDone, onUploadData);
    webServer().on("/modules/restart", HTTP_POST, onRestart);
    webServer().on("/modules/remove", HTTP_POST, onRemove);
    webServer().on("/modules.html", HTTP_GET, onModulesPage);
    webServer().on("/modules/file", HTTP_GET, onFile);
    webServer().on("/modules", HTTP_GET, onStatus);

    // the MODULES menu entry of the WebUI module
    wsRegister().onVisible([](JsonObject& root) {
        wsPayloadModule(root, STRING_VIEW("mod"));
    });

    webServer().on("/modules/on", HTTP_POST, onModulesOn);

    Directory dir;
    if (!readDirectory(dir)) {
        return;
    }

    if (offReason().length()) {
        for (size_t index = 0; index < dir.count; ++index) {
            status[index] = Status{State::Off, nullptr, dir.entries[index].offset, dir.entries[index].crc32};
        }
        DEBUG_MSG_P(PSTR("[MODULES] Off (%s), MODULES.ON starts them again\n"), offReason().c_str());
        return;
    }

    // providers of an API (the sensors) before its users, by the header's priority
    size_t order[DirectoryEntries];
    uint32_t priority[DirectoryEntries];
    for (size_t index = 0; index < dir.count; ++index) {
        Header header;
        priority[index] = ESP.flashRead(dir.entries[index].offset, reinterpret_cast<uint32_t*>(&header), sizeof(header))
            ? header.priority : 0xffffffff;
        size_t pos = index;
        while (pos && (priority[order[pos - 1]] > priority[index])) {
            order[pos] = order[pos - 1];
            --pos;
        }
        order[pos] = index;
    }

    for (size_t position = 0; position < dir.count; ++position) {
        const size_t index = order[position];
        const auto& entry = dir.entries[index];
        const auto* error = start(entry);
        if (!error) {
            status[index] = Status{State::Started, nullptr, entry.offset, entry.crc32};
            boot_flash[boot_flash_count++] = BootPiece{entry.offset, (entry.size + SectorSize - 1) & ~(SectorSize - 1)};
            Header header;
            ESP.flashRead(entry.offset, reinterpret_cast<uint32_t*>(&header), sizeof(header));
            if (header.entry) {
                running = true;
            } else {
                auto& out = data[data_count++];
                std::memcpy(out.name, entry.name, sizeof(out.name));
                out.data = reinterpret_cast<const uint8_t*>(FlashMapped + entry.offset + sizeof(Header));
                out.size = header.data_end;
            }
            DEBUG_MSG_P(PSTR("[MODULES] %s started (0x%05x, %u bytes)\n"), entry.name, entry.offset, entry.size);
            continue;
        }

        const bool stale = (strcmp_P("stale", error) == 0);
        status[index] = Status{stale ? State::Stale : State::Broken, error, entry.offset, entry.crc32};
        DEBUG_MSG_P(PSTR("[MODULES] %s not started: %s\n"), entry.name, error);
    }

    // the guard watches only while module code runs
    if (running) {
        guard.running = 1;
        espurnaRegisterLoop(guardLoop);
    }
}

bool modulesRunning() {
    return espurna::modules::running;
}

void modulesOnHome(AsyncWebServerRequest* request) {
    using namespace espurna::modules;
    if (!webAuthenticate(request)) {
        return request->requestAuthentication(systemHostname().c_str());
    }

    for (size_t index = 0; index < data_count; ++index) {
        if (std::strcmp(data[index].name, "webui") == 0) {
            auto* response = request->beginResponse_P(200, String(F("text/html")), data[index].data, data[index].size);
            response->addHeader(String(F("Content-Encoding")), String(F("gzip")));
            request->send(response);
            return;
        }
    }

    onModulesPage(request);
}

void modulesForget() {
    espurna::modules::Directory dir;
    if (espurna::modules::readDirectory(dir)) {
        espurna::modules::eraseDirectory();
    }
}

#endif // MODULES_SUPPORT
