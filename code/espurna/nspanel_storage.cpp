/*

NSPANEL MODULE - storage

The config in the flash the module keeps its data in (modulesData() of the head: 32 KB at the top of the module area,
placed by the Apply only while the module is installed, kept by the next Apply), the same on every chip. Two halves of
16 KB, each a header (a mark, a sequence number, the size, the CRC) and the compiled image of the config
(nspanel_config.h), nothing else: no JSON text is kept, GET /nspanel.json is written back from the image. A new image
goes into the half the current one is not in, straight from the compile as it is laid out (FlashImage: never one heap
block of it), its header last; the boot takes the valid half with the higher number. An upload that breaks off or does
not compile leaves the current one.

The image is read in place: modulesDataView() is the data as plain memory (the mapped flash); a chip that cannot give
that (encrypted or word-only flash) reads the image into one heap block.

A board that still keeps the JSON text of the old format, or an image of an older layout, compiles it once at boot
and keeps the image from then on. The heap budget of a compile and the built-in config below

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
constexpr uint32_t JsonMagic { 0x4350534e };  // "NSPC": the JSON text (the old format, compiled once at boot)
constexpr uint32_t ImageMagic { 0x4950534e }; // "NSPI": the compiled image

struct Header {
    uint32_t magic;
    uint32_t sequence;
    uint32_t size;
    uint32_t crc32;
};

static_assert(sizeof(Header) + build::ConfigMax <= Half, "");

bool is_mounted { false };

// the half the last open() bound a config to (it may be the running one): save() never writes there. -1 - none
int bound_half { -1 };

bool header(uint32_t half, Header& out) {
    return modulesDataRead(Module, half * Half, &out, sizeof(out))
        && ((out.magic == ImageMagic) || (out.magic == JsonMagic)) && out.size && (out.size <= build::ConfigMax);
}

// the valid halves, the higher sequence number first; returns how many
size_t halves(uint32_t (&order)[2], Header (&found)[2]) {
    Header first;
    Header second;
    const bool a = header(0, first);
    const bool b = header(1, second);

    size_t count = 0;
    const bool a_first = a && (!b || (static_cast<int32_t>(first.sequence - second.sequence) > 0));
    if (a_first) {
        order[count] = 0;
        found[count++] = first;
    }
    if (b) {
        order[count] = 1;
        found[count++] = second;
    }
    if (a && !a_first) {
        order[count] = 0;
        found[count++] = first;
    }

    return count;
}

// the valid half with the higher sequence number; false - none
bool current(uint32_t& half, Header& out) {
    uint32_t order[2];
    Header found[2];
    if (!halves(order, found)) {
        return false;
    }

    half = order[0];
    out = found[0];
    return true;
}

// the data of a half into `out` (size bytes), its CRC checked
bool readData(uint32_t half, const Header& header, uint8_t* out) {
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

// the data of a half as memory: the mapped flash, or a heap copy in 'owned'; nullptr - unreadable or damaged
const uint8_t* dataOf(uint32_t half, const Header& found, std::unique_ptr<uint8_t[]>& owned) {
    owned.reset();
    if (const auto* view = modulesDataView(Module)) {
        const auto* data = view + (half * Half) + sizeof(Header);
        return (modules::crc32Update(0, data, found.size) == found.crc32) ? data : nullptr;
    }

    owned.reset(new (std::nothrow) uint8_t[found.size + 1]);
    if (!owned || !readData(half, found, owned.get())) {
        owned.reset();
        return nullptr;
    }

    return owned.get();
}

// the JSON of an image of an older layout, NUL terminated, for a compile
void countJson(void* ctx, const char*, size_t length) {
    *static_cast<size_t*>(ctx) += length;
}

struct Fill {
    char* at;
};

void fillJson(void* ctx, const char* data, size_t length) {
    auto* fill = static_cast<Fill*>(ctx);
    std::memcpy(fill->at, data, length);
    fill->at += length;
}

bool imageJson(const uint8_t* data, std::unique_ptr<char[]>& text, size_t& size) {
    size = 0;
    writeJson(data, countJson, &size);
    text.reset(new (std::nothrow) char[size + 1]);
    if (!text) {
        return false;
    }

    Fill fill { text.get() };
    writeJson(data, fillJson, &fill);
    text[size] = '\0';
    return true;
}

// the text of a half (the old format, or an older image written back): compiled and saved as an image
bool rebuild(std::unique_ptr<char[]> text, size_t size, String& error, String& warnings) {
    return ParseResult::Ok == save(text.get(), size, error, warnings);
}

// the flash writes of an image: a multiple of 4 (modulesDataWrite())
constexpr size_t FlashChunk { 512 };

// The image straight into the half the running config is not read from (normally the other one than the current) as
// the compile lays it out: the half is erased at begin(), the bytes go through a FlashChunk buffer, the header with the
// CRC is written last, so a failure or a power cut leaves the current half as the valid one
class FlashImage : public ImageSink {
public:
    bool begin(size_t size, String& error) override {
        if (!is_mounted) {
            error = F("no flash data of the module");
            return false;
        }
        if (!size || (size > build::ConfigMax)) {
            error = F("config too large for the flash data of the module");
            return false;
        }

        _buffer.reset(new (std::nothrow) uint8_t[FlashChunk]);
        if (!_buffer) {
            error = F("out of memory (flash buffer), retry");
            return false;
        }

        uint32_t half = 0;
        Header found {};
        const bool has = current(half, found);
        _target = (bound_half >= 0) ? static_cast<uint32_t>(1 - bound_half) : (has ? (1 - half) : 0);
        _sequence = has ? (found.sequence + 1) : 1;
        _size = size;
        _done = 0;
        _fill = 0;
        _crc = 0;

        if (!modulesDataErase(Module, _target * Half, Half)) {
            error = F("cannot erase the flash data of the module");
            return false;
        }

        return true;
    }

    bool write(const uint8_t* data, size_t length) override {
        if (!_buffer || ((_done + _fill + length) > _size)) {
            return false;
        }

        while (length) {
            const size_t part = std::min(length, FlashChunk - _fill);
            std::memcpy(_buffer.get() + _fill, data, part);
            _crc = modules::crc32Update(_crc, data, part);
            _fill += part;
            data += part;
            length -= part;
            if ((_fill == FlashChunk) && !flush()) {
                return false;
            }
        }

        return true;
    }

    bool end(String& error) override {
        if (!_buffer || !flush() || (_done != _size)) {
            error = F("cannot write the compiled config");
            return false;
        }

        alignas(4) Header out { ImageMagic, _sequence, static_cast<uint32_t>(_size), _crc };
        if (!modulesDataWrite(Module, _target * Half, &out, sizeof(out))) {
            error = F("cannot write the compiled config");
            return false;
        }

        return true;
    }

private:
    bool flush() {
        if (!_fill) {
            return true;
        }

        const size_t aligned = (_fill + 3) & ~static_cast<size_t>(3);
        std::memset(_buffer.get() + _fill, 0xff, aligned - _fill);
        if (!modulesDataWrite(Module, (_target * Half) + sizeof(Header) + _done, _buffer.get(), aligned)) {
            return false;
        }

        _done += _fill;
        _fill = 0;
        return true;
    }

    std::unique_ptr<uint8_t[]> _buffer;
    uint32_t _target { 0 };
    uint32_t _sequence { 0 };
    size_t _size { 0 };
    size_t _done { 0 };
    size_t _fill { 0 };
    uint32_t _crc { 0 };
};

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

// the newest usable half bound to 'out'. A half of the old format (JSON text) or an image of an older layout is
// compiled once and saved as an image, then that is opened
bool open(Config& out, String& error, String& warnings) {
    if (!is_mounted) {
        error = F("no flash data of the module");
        return false;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        uint32_t order[2];
        Header found[2];
        const size_t count = halves(order, found);
        if (!count) {
            error = F("no saved config");
            return false;
        }

        bool rebuilt = false;
        for (size_t index = 0; (index < count) && !rebuilt; ++index) {
            const auto half = order[index];
            const auto& head = found[index];

            std::unique_ptr<uint8_t[]> owned;
            const auto* data = dataOf(half, head, owned);
            if (!data) {
                error = F("the saved config is damaged (CRC)");
                DEBUG_MSG_P(PSTR("[NSPANEL] Config half %u damaged\n"), half);
                continue;
            }

            if (head.magic == ImageMagic) {
                if (::espurna::nspanel::bind(out, data, head.size, owned)) {
                    bound_half = half;
                    return true;
                }
                if (!treeReadable(data, head.size)) {
                    error = F("the saved config is of an unknown layout");
                    continue;
                }

                // an older layout: its tree back to JSON, compiled again
                std::unique_ptr<char[]> text;
                size_t size = 0;
                if (!imageJson(data, text, size)) {
                    error = F("out of memory while converting the saved config");
                    return false;
                }
                owned.reset();
                DEBUG_MSG_P(PSTR("[NSPANEL] Config of an older layout, compiling it again\n"));
                if (!rebuild(std::move(text), size, error, warnings)) {
                    return false;
                }
                rebuilt = true;
                continue;
            }

            // the JSON text of the old format: compiled once, the image kept from now on
            std::unique_ptr<char[]> text(new (std::nothrow) char[head.size + 1]);
            if (!text) {
                error = F("out of memory while converting the saved config");
                return false;
            }
            std::memcpy(text.get(), data, head.size);
            text[head.size] = '\0';
            owned.reset();
            DEBUG_MSG_P(PSTR("[NSPANEL] Config saved as JSON text, compiling it once\n"));
            if (!rebuild(std::move(text), head.size, error, warnings)) {
                return false;
            }
            rebuilt = true;
        }

        if (!rebuilt) {
            return false;
        }
    }

    return false;
}

// the text compiled straight into the half the running config is not read from (FlashImage)
ParseResult save(char* text, size_t length, String& error, String& warnings) {
    if (!is_mounted) {
        error = F("no flash data of the module");
        return ParseResult::WriteFailed;
    }

    FlashImage image;
    return compileText(text, length, image, error, warnings);
}

void info(Print& out) {
    uint32_t half;
    Header found;
    if (!is_mounted) {
        out.print(F("Config: no flash data of the module\n"));
    } else if (current(half, found)) {
        out.printf_P(PSTR("Config: %s of %u bytes in the flash data of the module (half %u, #%u), %s\n"),
            (found.magic == ImageMagic) ? "image" : "JSON text", found.size, half, found.sequence,
            modulesDataView(Module) ? "read in place" : "a heap copy");
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
    // the MQTT module's setting mqttBuffer (mqtt.cpp), read the same way: no call into the module
    out.mqtt_buffer = std::clamp<size_t>(getSetting("mqttBuffer", static_cast<size_t>(MQTT_BUFFER_MAX_SIZE)), 256, 8192);
#endif
    return out;
}

// Free heap a compile needs: the JSON nodes (in blocks, +1/32) plus the draft (build::ConfigDraftPerKb per KB of text
// and build::ConfigDraftFixed) plus what WiFi / lwIP must keep. The image is not in the heap (it goes to its sink as it
// is laid out), no block above build::CompileBlock is taken
bool heapAvailable(size_t json_size, size_t length, String& error) {
    const size_t need = json_size + (json_size / 32)
        + ((length * build::ConfigDraftPerKb) / 1024) + build::ConfigDraftFixed
        + build::ConfigHeapReserve;
    const size_t block_need = build::CompileBlock + 64 + build::ConfigBlockReserve;
    const auto heap = systemHeapStats();
    const size_t free_heap = heap.available;
    const size_t block = heap.usable;
    if ((free_heap >= need) && (block >= block_need)) {
        return true;
    }

    char buf[96];
    snprintf_P(buf, sizeof(buf), PSTR("low memory (free %u, largest block %u, need %u / %u), retry"),
        free_heap, block, need, block_need);
    error = buf;
    return false;
}

// heap check + compile, for text the caller does not need afterwards
ParseResult compileText(char* text, size_t length, ImageSink& out, String& error, String& warnings) {
    const size_t json_size = jsonBufferSize(text, length);
    if (!heapAvailable(json_size, length, error)) {
        return ParseResult::LowMemory;
    }

    return compile(text, parseLimits(), out, error, warnings);
}

// built-in config, compiled from a heap copy into a heap image (nothing is written to the flash)
ParseResult parseDefault(Config& out, String& error, String& warnings) {
    const size_t length = strlen_P(DefaultConfig);
    std::unique_ptr<char[]> text(new (std::nothrow) char[length + 1]);
    if (!text) {
        error = F("out of memory");
        return ParseResult::LowMemory;
    }

    memcpy_P(text.get(), DefaultConfig, length + 1);

    ImageBlock image;
    const auto result = compileText(text.get(), length, image, error, warnings);
    if (result != ParseResult::Ok) {
        return result;
    }

    const auto* data = image.data.get();
    if (!::espurna::nspanel::bind(out, data, image.size, image.data)) {
        error = F("built-in config: bad image");
        return ParseResult::Invalid;
    }

    return ParseResult::Ok;
}

} // namespace nspanel
} // namespace espurna

#endif // NSPANEL_SUPPORT
