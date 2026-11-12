/*

NSPANEL MODULE - storage

LittleFS on the existing data/spiffs partition: /nspanel.json, written through a temporary file; the
heap budget of a config parse and the built-in config

*/

#include "espurna.h"

#if NSPANEL_SUPPORT

#include "nspanel_core.h"

#if MQTT_SUPPORT
#include "mqtt.h"
#endif

#if RELAY_SUPPORT
#include "relay.h"
#endif

#include <LittleFS.h>

#include <esp_partition.h>

#include <memory>
#include <new>

namespace espurna {
namespace nspanel {
namespace storage {
namespace {

bool is_mounted { false };
String label;

} // namespace

bool mount() {
    if (is_mounted) {
        return true;
    }

    const auto* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (!part) {
        DEBUG_MSG_P(PSTR("[NSPANEL] No data/spiffs partition\n"));
        return false;
    }

    label = part->label;
    if (!LittleFS.begin(false, "/littlefs", 4, part->label)) {
        DEBUG_MSG_P(PSTR("[NSPANEL] LittleFS mount failed on '%s', formatting\n"), part->label);
        if (!LittleFS.begin(true, "/littlefs", 4, part->label)) {
            DEBUG_MSG_P(PSTR("[NSPANEL] LittleFS format failed\n"));
            return false;
        }
    }

    is_mounted = true;
    DEBUG_MSG_P(PSTR("[NSPANEL] LittleFS on '%s' (%u / %u bytes used)\n"),
        part->label, LittleFS.usedBytes(), LittleFS.totalBytes());

    return true;
}

bool read(String& out) {
    if (!is_mounted || !LittleFS.exists(build::ConfigPath)) {
        return false;
    }

    File file = LittleFS.open(build::ConfigPath, "r");
    if (!file) {
        return false;
    }

    const size_t size = file.size();
    if (!size || (size > build::ConfigMax)) {
        file.close();
        return false;
    }

    out = String();
    out.reserve(size);
    char buf[256];
    while (file.available()) {
        const auto len = file.readBytes(buf, sizeof(buf));
        if (!len) {
            break;
        }
        out.concat(buf, len);
    }
    file.close();

    return out.length() == size;
}

// NUL terminated copy of the file for an in-place parse, exactly size + 1 bytes
bool read(std::unique_ptr<char[]>& out, size_t& out_size) {
    out.reset();
    out_size = 0;
    if (!is_mounted || !LittleFS.exists(build::ConfigPath)) {
        return false;
    }

    File file = LittleFS.open(build::ConfigPath, "r");
    if (!file) {
        return false;
    }

    const size_t size = file.size();
    if (!size || (size > build::ConfigMax)) {
        file.close();
        return false;
    }

    out.reset(new (std::nothrow) char[size + 1]);
    if (!out) {
        file.close();
        return false;
    }

    const auto len = file.readBytes(out.get(), size);
    file.close();
    if (len != size) {
        out.reset();
        return false;
    }

    out[size] = '\0';
    out_size = size;
    return true;
}

void removeTmp() {
    if (is_mounted) {
        LittleFS.remove(build::ConfigTmpPath);
    }
}

// the upload is saved before the in-place parse destroys it, and only renamed to
// ConfigPath when it is valid (ref. commitTmp())
bool writeTmp(const char* data, size_t size) {
    if (!is_mounted) {
        return false;
    }

    File file = LittleFS.open(build::ConfigTmpPath, "w");
    if (!file) {
        return false;
    }

    const auto written = file.write(reinterpret_cast<const uint8_t*>(data), size);
    file.close();
    if (written != size) {
        removeTmp();
        return false;
    }

    return true;
}

bool commitTmp() {
    if (!LittleFS.rename(build::ConfigTmpPath, build::ConfigPath)) {
        LittleFS.remove(build::ConfigPath);
        return LittleFS.rename(build::ConfigTmpPath, build::ConfigPath);
    }

    return true;
}

bool mounted() {
    return is_mounted;
}

bool exists() {
    return is_mounted && LittleFS.exists(build::ConfigPath);
}

void info(Print& out) {
    if (is_mounted) {
        out.printf_P(PSTR("FS: LittleFS on '%s', %u / %u bytes used\n"),
            label.c_str(), LittleFS.usedBytes(), LittleFS.totalBytes());
    } else {
        out.print(F("FS: not mounted\n"));
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
    const size_t free_heap = ESP.getFreeHeap();
    const size_t block = ESP->getMaxAllocHeap();
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
