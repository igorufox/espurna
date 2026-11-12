/*

NSPANEL MODULE - storage

The config (nspanel.json) in the flash the module keeps its data in (modulesData() of the head: 32 KB at the top of
the module area, placed by the Apply only while the module is installed, kept by the next Apply), the same on every
chip. Two halves of 16 KB: a new config goes into the half the current one is not in, its header (a sequence number,
the size, the CRC) last, after the parse said it is valid (commitTmp()); the boot takes the valid half with the higher
number. An upload that breaks off or does not parse leaves the current one. The heap budget of a config parse and the
built-in config below

*/

#include "espurna.h"

#if NSPANEL_SUPPORT

#include "nspanel_core.h"
#include "modules.h"
#include "modules_core.h"
#include "system.h"

#if MQTT_SUPPORT
#include "mqtt.h"
#endif

#if RELAY_SUPPORT
#include "relay.h"
#endif

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

namespace espurna {
namespace nspanel {
namespace storage {
namespace {

constexpr char Module[] = "nspanel";
constexpr uint32_t Half { 16384 };
constexpr uint32_t Magic { 0x4350534e }; // "NSPC"

struct Header {
    uint32_t magic;
    uint32_t sequence;
    uint32_t size;
    uint32_t crc32;
};

static_assert(sizeof(Header) + build::ConfigMax <= Half, "");

bool is_mounted { false };

// the upload: the half it goes into, what its header will say
struct Pending {
    uint32_t half;
    uint32_t sequence;
    uint32_t size;
    uint32_t crc32;
    bool written;
};

Pending pending {};

bool header(uint32_t half, Header& out) {
    return modulesDataRead(Module, half * Half, &out, sizeof(out))
        && (out.magic == Magic) && out.size && (out.size <= build::ConfigMax);
}

// the valid half with the higher sequence number; false - none
bool current(uint32_t& half, Header& out) {
    Header first;
    Header second;
    const bool a = header(0, first);
    const bool b = header(1, second);
    if (!a && !b) {
        return false;
    }

    if (a && (!b || (static_cast<int32_t>(first.sequence - second.sequence) > 0))) {
        half = 0;
        out = first;
    } else {
        half = 1;
        out = second;
    }

    return true;
}

// the data of a half into `out` (size bytes), its CRC checked
bool readData(uint32_t half, const Header& header, char* out) {
    alignas(4) uint8_t chunk[256];
    uint32_t crc = 0;
    for (uint32_t done = 0; done < header.size; done += sizeof(chunk)) {
        const uint32_t length = std::min<uint32_t>(sizeof(chunk), header.size - done);
        if (!modulesDataRead(Module, (half * Half) + sizeof(Header) + done, chunk, (length + 3) & ~3u)) {
            return false;
        }
        std::memcpy(out + done, chunk, length);
        crc = modules::crc32Update(crc, chunk, length);
    }

    return crc == header.crc32;
}

} // namespace

bool mount() {
    is_mounted = modulesDataSize(Module) >= (2 * Half);
    if (!is_mounted) {
        DEBUG_MSG_P(PSTR("[NSPANEL] No flash data of the module (Apply it again)\n"));
    }

    return is_mounted;
}

bool mounted() {
    return is_mounted;
}

bool exists() {
    uint32_t half;
    Header out;
    return is_mounted && current(half, out);
}

bool read(std::unique_ptr<char[]>& out, size_t& out_size) {
    out.reset();
    out_size = 0;

    uint32_t half;
    Header found;
    if (!is_mounted || !current(half, found)) {
        return false;
    }

    out.reset(new (std::nothrow) char[found.size + 1]);
    if (!out) {
        return false;
    }

    if (!readData(half, found, out.get())) {
        out.reset();
        return false;
    }

    out[found.size] = '\0';
    out_size = found.size;
    return true;
}

bool read(String& out) {
    std::unique_ptr<char[]> text;
    size_t size;
    if (!read(text, size)) {
        return false;
    }

    out = String();
    out.reserve(size);
    out.concat(text.get(), size);
    return out.length() == size;
}

void removeTmp() {
    pending = Pending{};
}

// the upload into the other half before the in-place parse destroys it; the header only with commitTmp()
bool writeTmp(const char* data, size_t size) {
    pending = Pending{};
    if (!is_mounted || !size || (size > build::ConfigMax)) {
        return false;
    }

    uint32_t half = 0;
    Header found {};
    const bool has = current(half, found);
    pending.half = has ? (1 - half) : 0;
    pending.sequence = has ? (found.sequence + 1) : 1;
    pending.size = size;

    if (!modulesDataErase(Module, pending.half * Half, Half)) {
        return false;
    }

    alignas(4) uint8_t chunk[256];
    uint32_t crc = 0;
    for (uint32_t done = 0; done < size; done += sizeof(chunk)) {
        const uint32_t length = std::min<uint32_t>(sizeof(chunk), size - done);
        std::memset(chunk, 0xff, sizeof(chunk));
        std::memcpy(chunk, data + done, length);
        crc = modules::crc32Update(crc, chunk, length);
        if (!modulesDataWrite(Module, (pending.half * Half) + sizeof(Header) + done, chunk, (length + 3) & ~3u)) {
            return false;
        }
    }

    pending.crc32 = crc;
    pending.written = true;
    return true;
}

bool commitTmp() {
    if (!pending.written) {
        return false;
    }

    alignas(4) Header out { Magic, pending.sequence, pending.size, pending.crc32 };
    const bool ok = modulesDataWrite(Module, pending.half * Half, &out, sizeof(out));
    pending = Pending{};
    return ok;
}

void info(Print& out) {
    uint32_t half;
    Header found;
    if (!is_mounted) {
        out.print(F("Config: no flash data of the module\n"));
    } else if (current(half, found)) {
        out.printf_P(PSTR("Config: %u bytes in the flash data of the module (half %u, #%u)\n"),
            found.size, half, found.sequence);
    } else {
        out.print(F("Config: none saved, the built-in one\n"));
    }
}

} // namespace storage

namespace {

PROGMEM_STRING(DefaultConfig,
    "{\"pages\":[{\"title\":\"NSPanel\",\"items\":["
    "{\"name\":\"Relay 1\",\"icon\":\"light-switch\",\"relay\":0},"
    "{\"name\":\"Relay 2\",\"icon\":\"light-switch\",\"relay\":1}]}],"
    "\"buttons\":[{\"relay\":0},{\"relay\":1}]}");

} // namespace

const char* defaultConfig() {
    return DefaultConfig;
}

Limits parseLimits() {
    Limits out;
#if RELAY_SUPPORT
    out.relays = relayCount();
#endif
#if MQTT_SUPPORT
    out.mqtt_buffer = MQTT_BUFFER_MAX_SIZE;
#endif
    return out;
}

// Free heap a parse needs: the JSON nodes plus the Config being built (Strings, entities,
// items: about 3 bytes per byte of text) plus what WiFi / lwIP / TLS must keep
bool heapAvailable(size_t json_size, size_t length, String& error) {
    const size_t need = json_size + (build::ConfigHeapPerByte * length) + build::ConfigHeapReserve;
    const auto heap = systemHeapStats();
    const size_t free_heap = heap.available;
    const size_t block = heap.usable;
    if ((free_heap >= need) && (block >= (json_size + build::ConfigBlockReserve))) {
        return true;
    }

    char buf[96];
    snprintf_P(buf, sizeof(buf), PSTR("low memory (free %u, largest block %u, need %u / %u), retry"),
        free_heap, block, need, json_size + build::ConfigBlockReserve);
    error = buf;
    return false;
}

// heap check + parse, for text the caller does not need afterwards
ParseResult parseConfig(char* text, size_t length, Config& out, String& error, String& warnings) {
    const size_t json_size = jsonBufferSize(text, length);
    if (!heapAvailable(json_size, length, error)) {
        return ParseResult::LowMemory;
    }

    return parseInPlace(text, json_size, parseLimits(), out, error, warnings);
}

// built-in config, parsed from a heap copy (in place, like the file)
ParseResult parseDefault(Config& out, String& error, String& warnings) {
    const size_t length = strlen_P(DefaultConfig);
    std::unique_ptr<char[]> text(new (std::nothrow) char[length + 1]);
    if (!text) {
        error = F("out of memory");
        return ParseResult::LowMemory;
    }

    memcpy_P(text.get(), DefaultConfig, length + 1);
    return parseConfig(text.get(), length, out, error, warnings);
}

} // namespace nspanel
} // namespace espurna

#endif // NSPANEL_SUPPORT
