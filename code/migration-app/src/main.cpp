/*

ESPurna migration (ESP8266, 1 MB boards)

The way over from the old ESPurna (1.15 ... 1.22, one image with everything in it) to the head + modules on 1 MB
boards. An old image takes only a plain .bin / .bin.gz on its /upgrade (not the .ota), and its ~800 KB leave ~200 KB of
free space, too little for the gzip of the head, so the old image takes this one (espurna-esp8266-1m-migration.bin.gz,
uploaded to /upgrade), boots it, and this one downloads the head's image (.bin.gz, 1 MB layout) and writes it. The
head itself updates with one /upgrade and never comes here. Same idea as
code/safeboot-app of ESP32, but here the migration image replaces the full image: there is no ESPurna to go back to until a
full image is written. Plain Arduino core (core 3.1.2 + NONOS SDK 3.0.5, the 1 MB layout of ESPurna), no ESPurna code,
every byte counts (the gzip of this image has to fit next to the biggest full image):
- the ESPurna settings are read in place: the settings pool of ESPurna (espurna/libs/settings_rotate.h, sectors
  251 and 250, the copy with the valid sum and the newest counter), the Embedis key-value records. Wi-Fi ssid0/pass0, ssid1/pass1, static
  ip0/gw0/mask0/dns0, hostname, adminPass, webFwUrl; no settings of its own
- the request of the full image (POST /ota/fetch there, ota_fetch.cpp): otaFetchUrl + otaFetchSha256. Taken once:
  erased (and a provisional "failed: ..." result stored) before anything else, then Wi-Fi (up to 3 minutes), the
  download (http:// or https://), SHA-256 check, Update (gzip is unpacked by eboot at the next boot). The result goes
  to otaFetchResult ("ok: ..." / "failed: ..."), which the full image shows in /ota/status; reboot on success
- settings writes: the same record edits as ESPurna (settings_embedis.h, set / del), always into the first sector
  of the pool (251) with the counter increased, rotation off as ESPurna does around an update: the image being
  downloaded overwrites the second one (250, the last sector of the free space). Records that do not end the way
  ESPurna leaves them (0xffff after the last one) are never written over
- otherwise, and after a failure: a page on port 80 (user admin + adminPass, HTTP Digest with realm = hostname as
  the ESPurna WebUI, so the browser answers with what it has). Status, last result, the releases of webFwUrl (read by
  the browser, GitHub Pages sends CORS *) with the 1 MB images that fit, a click stores the request and reboots into
  it; a file upload to /upgrade (.bin or .bin.gz, raw body or multipart as ESPurna's /upgrade); GET /ota/status and
  POST /ota/fetch in the shape of the full image (fwinstall.mjs)
- no Wi-Fi in 30 s: own access point <hostname>-MIGRATION (WPA2, admin password), 192.168.4.1. Every 5 minutes without
  anybody on it, the board tries the Wi-Fi again (a router back after a power cut); 5 minutes without the Wi-Fi:
  the access point again
- log on the serial TX pin (GPIO1, 115200), written to the UART registers (no Serial, no printf of newlib)

The download client and the web server are the ones of ESPurna (espurna/libs/net_impl.h, every chip, lwIP), their
one implementation compiled here: TLS is raw BearSSL, TLS 1.2, static RSA key exchange + AES-128-GCM (the head's
part, what GitHub Pages offers that takes the least code; no tls module here). The server certificate is not
validated (no CA store, no clock): only its RSA key is taken, the file is checked against its SHA-256 instead, which
the browser took from firmware.json over validated https. SHA-256 is required. The checks of the request
(URL, SHA-256) are espurna/libs/fetch_check.h, the same as ESPurna and the ESP32 safeboot; the settings records are
read with espurna/libs/embedis_records.h, the same as code/safeboot-app

*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <eboot_command.h>
#include <spi_flash.h>
#include <lwip/ip4_addr.h>

#ifndef MIGRATION_LOG
#define MIGRATION_LOG 1
#endif

#include <bearssl.h>

#include <memory>

#include "../../espurna/libs/embedis_records.h"
#include "../../espurna/libs/settings_rotate.h"
#include "../../espurna/libs/fetch_check.h"
#include "../../espurna/libs/text.h"

// -----------------------------------------------------------------------------
// log
// -----------------------------------------------------------------------------

namespace {

#if MIGRATION_LOG
void put(char c) {
    while (((USS(0) >> USTXC) & 0xff) >= 0x7f) {
    }
    USF(0) = c;
}

// format in flash or RAM
void logv(const char* fmt, va_list args) {
    char format[96];
    strncpy_P(format, fmt, sizeof(format) - 1);
    format[sizeof(format) - 1] = '\0';

    char out[200];
    ets_vsnprintf(out, sizeof(out), format, args);
    for (const char* ptr = "[MIGRATION] "; *ptr; ++ptr) {
        put(*ptr);
    }
    for (const char* ptr = out; *ptr; ++ptr) {
        put(*ptr);
    }
    put('\r');
    put('\n');
}

void logf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logv(fmt, args);
    va_end(args);
}

#define LOG(fmt, ...) logf(PSTR(fmt), ##__VA_ARGS__)
#else
#define LOG(...) do {} while (false)
#endif

} // namespace

// The SDK formatter (ets_vsnprintf, linked anyway; %d %u %x %s %c) instead of the one of newlib (~3 KB): the Core
// uses vsnprintf for its panic / postmortem output only
extern "C" int vsnprintf(char* buffer, size_t size, const char* fmt, va_list args) {
    return ets_vsnprintf(buffer, size, fmt, args);
}

// the one http:// and https:// client of ESPurna (ota_fetch.cpp, nspanel), every chip: its one implementation. One
// thread here, no lock
namespace migration {
struct NoLock {
    void lock() {}
    void unlock() {}
};
} // namespace migration

#define HTTP_CLIENT_AGENT "ESPurna-migration"
#define NET_LOG(fmt, ...) LOG(fmt, ##__VA_ARGS__)
#define NET_LOCK ::migration::NoLock
#include "../../espurna/libs/net_impl.h"
#include "../../espurna/libs/http_client_impl.h"
#include "../../espurna/libs/image_receiver.h"
#include "../../espurna/libs/tls_setup.h"

// no modules here: the base TLS of libs/tls_setup.h only (static RSA + AES-128-GCM, what GitHub Pages offers)
namespace espurna {
namespace tls {

Failure start(Engine& engine, const char* host, const Options& options, const uint8_t* seed, size_t seed_len,
    unsigned char* input, size_t input_len, unsigned char* output, size_t output_len)
{
    return setup::start(engine, host, options, seed, seed_len, input, input_len, output, output_len);
}

bool extend(br_ssl_client_context*) {
    return false;
}

void* key(const uint8_t*, size_t, br_x509_pkey*) {
    return nullptr;
}

const br_x509_class** caCheck(const char*, void**) {
    return nullptr;
}

} // namespace tls
} // namespace espurna

namespace {

// the release image (ESPURNA_IMAGE of the ini, as the three chips have it)
constexpr char Image[] = ESPURNA_IMAGE;
constexpr char DefaultPassword[] = "fibonacci";
constexpr char DefaultManifest[] = "https://igorufox.github.io/espurna/firmware.json";

constexpr unsigned long ConnectWindow { 30000 };
constexpr unsigned long RequestConnectWindow { 3 * 60 * 1000 };
constexpr unsigned long FallbackAfter { 5 * 60 * 1000 };

namespace keys {

constexpr char Url[] = "otaFetchUrl";
constexpr char Sha256[] = "otaFetchSha256";
constexpr char Result[] = "otaFetchResult";

} // namespace keys

using espurna::fetch_check::validSha256;
using espurna::fetch_check::validUrl;
using espurna::http::number;

// lower case hex of up to 32 bytes (a SHA-256), espurna::http::hex() into a String
String hex(const uint8_t* data, size_t len) {
    char out[2 * 32 + 1];
    espurna::http::hex(data, std::min<size_t>(len, 32), out);
    return String(out);
}


// -----------------------------------------------------------------------------
// ESPurna settings
// -----------------------------------------------------------------------------

// One 4 KB sector of the settings pool of ESPurna (the format of espurna/libs/settings_rotate.h, the same code as
// ESPurna's storage_eeprom.cpp): sum and counter at 11..13, the crash record up to 256 (crash.cpp), the Embedis records
// from the end of the sector down to 256 (settings.cpp), read and walked with espurna/libs/embedis_records.h
constexpr uint16_t ReservedEnd { espurna::settings_rotate::ReservedEnd };
constexpr uint16_t KvBegin { 256 };
constexpr uint16_t KvEnd { SPI_FLASH_SEC_SIZE };

using Record = espurna::embedis_records::Record;

struct Pair {
    const char* key;
    String value;
};

class Storage {
public:
    void load() {
        // as ESPurna (arch/esp8266/storage_flash_esp8266.cpp): the base sector under the SDK's 4, 2 sectors (251, 250)
        // with the 1 MB layout
        _base = (ESP.getFlashChipSize() / SPI_FLASH_SEC_SIZE) - 5;
        _pool = (_base > 250) ? 2 : 1;

        const auto found = espurna::settings_rotate::find(_pool, _data,
            [&](size_t index, uint8_t* out) {
                return ESP.flashRead((_base - index) * SPI_FLASH_SEC_SIZE, reinterpret_cast<uint32_t*>(out),
                    SPI_FLASH_SEC_SIZE);
            });

        _valid = found.valid;
        _counter = found.counter;
        _current = _base - found.index;
        LOG("Settings in sector %d (pool of %d): %s", _current, _pool, _valid ? "valid" : "none");
    }

    int current() const {
        return _current;
    }

    String get(const char* key) const {
        String out;
        Record record;
        if (_valid && espurna::embedis_records::find(_data, KvBegin, KvEnd, key, record)) {
            out.concat(reinterpret_cast<const char*>(_data + record.value), record.value_len);
        }

        return out;
    }

    // ESPurna set() and del() at once: the records of `drop` and of the keys of `pairs` go, the others move towards
    // the end of the sector in the same order, the pairs follow them
    bool write(const Pair* pairs, size_t count, const char* drop = nullptr, const char* drop2 = nullptr) {
        if (!_valid) {
            // no valid copy, ESPurna starts with empty settings as well
            memset(_data + ReservedEnd, 0xff, KvEnd - ReservedEnd);
        }

        auto dropped = [&](const Record& record) {
            if ((drop && match(record, drop)) || (drop2 && match(record, drop2))) {
                return true;
            }

            for (size_t index = 0; index < count; ++index) {
                if (match(record, pairs[index].key)) {
                    return true;
                }
            }

            return false;
        };

        // nothing is changed unless the records end with the free space marker and the new ones fit
        size_t pos { KvEnd };
        size_t kept { 0 };
        Record record;
        bool clean { false };
        while (next(pos, record, clean)) {
            if (!dropped(record)) {
                kept += record.end - record.begin;
            }
        }

        size_t need { 0 };
        for (size_t index = 0; index < count; ++index) {
            need += 4 + strlen(pairs[index].key) + pairs[index].value.length();
        }

        if (!clean || ((kept + need + 2) > static_cast<size_t>(KvEnd - KvBegin))) {
            LOG("Settings not written: %s", clean ? "no room" : "unknown layout");
            return false;
        }

        const size_t low { pos };
        size_t out { KvEnd };
        pos = KvEnd;
        while (next(pos, record, clean)) {
            if (!dropped(record)) {
                const size_t size = record.end - record.begin;
                memmove(_data + out - size, _data + record.begin, size);
                out -= size;
            }
        }

        memset(_data + low, 0xff, out - low);

        for (size_t index = 0; index < count; ++index) {
            const auto& pair = pairs[index];
            const size_t key_len = strlen(pair.key);
            const size_t value_len = pair.value.length();

            out -= 4 + key_len + value_len;
            auto* ptr = _data + out;
            memcpy(ptr, pair.value.c_str(), value_len);
            ptr += value_len;
            *ptr++ = value_len >> 8;
            *ptr++ = value_len & 0xff;
            memcpy(ptr, pair.key, key_len);
            ptr += key_len;
            *ptr++ = key_len >> 8;
            *ptr = key_len & 0xff;
        }

        _data[out - 2] = 0xff;
        _data[out - 1] = 0xff;

        // the first sector of the pool, counter + 1: newer than the second one whatever is left of it
        const bool ok = commit();
        _valid = _valid || ok;

        LOG("Settings written to sector %d: %s", current(), ok ? "ok" : "FAILED");
        return ok;
    }

private:
    bool match(const Record& record, const char* key) const {
        return espurna::embedis_records::match(_data, record, key);
    }

    // the record below `pos`. false at the end: `clean` when it is the free space marker
    bool next(size_t& pos, Record& out, bool& clean) const {
        return espurna::embedis_records::next(_data, KvBegin, pos, out, clean);
    }

    bool commit() {
        espurna::settings_rotate::seal(_data, static_cast<uint8_t>(_counter + 1));
        if (!ESP.flashEraseSector(_base)
            || !ESP.flashWrite(_base * SPI_FLASH_SEC_SIZE, reinterpret_cast<uint32_t*>(_data), SPI_FLASH_SEC_SIZE))
        {
            return false;
        }

        ++_counter;
        _current = _base;
        return true;
    }

    alignas(4) uint8_t _data[KvEnd];
    uint32_t _base { 0 };
    uint8_t _pool { 1 };
    uint8_t _counter { 0 };
    int _current { 0 };
    bool _valid { false };
};

Storage storage;

struct Config {
    String ssid[2];
    String pass[2];
    String ip;
    String gw;
    String mask;
    String dns;
    String hostname;
    String password;
    String manifest;
};

Config config;

String last_result;

void storeResult(const String& value) {
    last_result = value;
    const Pair pair { keys::Result, value };
    storage.write(&pair, 1);
    LOG("Result: %s", value.c_str());
}

// -----------------------------------------------------------------------------
// image writer
// -----------------------------------------------------------------------------

} // namespace

extern "C" uint32_t _FS_start;

namespace {

// The migration image's target of the one image receiver (espurna/libs/image_receiver.h). Instead of the Updater of the Core
// (~4 KB with its MD5, signature and LED code), the same placement and eboot command: the file goes to the end of the
// free space (right below the EEPROM sector 251, over 250), sector by sector, SHA-256 on the way. end() reads the
// written file back and compares its SHA-256 (a staged image with holes in flash once bricked a 1 MB board, the stream
// checksum had said OK), only then eboot is told to copy it (gzip unpacked)
class Writer final : public espurna::image::Target {
public:
    // the size has to be known: the file is placed at the end of the free space
    const char* begin(const uint8_t* data, size_t len, size_t size) override {
        // ESP8266 application or gzip (unpacked by eboot)
        const bool gzip = (len > 1) && (data[0] == 0x1f) && (data[1] == 0x8b);
        if (!gzip && ((len < 4) || (data[0] != 0xe9))) {
            return "not an ESP8266 image (no 0xE9 or gzip magic)";
        }

        if (!size) {
            return "the size of the image is not known";
        }

        const uint32_t end = reinterpret_cast<uint32_t>(&_FS_start) - 0x40200000;
        const uint32_t used = (ESP.getSketchSize() + SPI_FLASH_SEC_SIZE - 1) & ~(SPI_FLASH_SEC_SIZE - 1);
        const uint32_t rounded = (size + SPI_FLASH_SEC_SIZE - 1) & ~(SPI_FLASH_SEC_SIZE - 1);
        if (rounded > end - used) {
            return "the file does not fit in the free space";
        }

        _buffer.reset(new (std::nothrow) uint8_t[SPI_FLASH_SEC_SIZE]);
        if (!_buffer) {
            return "out of memory";
        }

        _start = end - rounded;
        _size = size;
        _written = 0;
        _len = 0;
        br_sha256_init(&_sha);
        return nullptr;
    }

    const char* write(const uint8_t* data, size_t len) override {
        br_sha256_update(&_sha, data, len);
        while (len) {
            const size_t size = std::min(len, SPI_FLASH_SEC_SIZE - _len);
            memcpy(_buffer.get() + _len, data, size);
            _len += size;
            data += size;
            len -= size;
            if ((_len == SPI_FLASH_SEC_SIZE) || (_written + _len == _size)) {
                const char* error = flush();
                if (error) {
                    return error;
                }
            }
        }

        return nullptr;
    }

    // the whole file arrived (the receiver checked its size, the client its SHA-256): read back, then eboot
    const char* end(size_t) override {
        uint8_t digest[br_sha256_SIZE];
        br_sha256_out(&_sha, digest);

        br_sha256_init(&_sha);
        for (uint32_t offset = 0; offset < _size; offset += SPI_FLASH_SEC_SIZE) {
            const uint32_t size = std::min<uint32_t>(_size - offset, SPI_FLASH_SEC_SIZE);
            if (spi_flash_read(_start + offset, reinterpret_cast<uint32_t*>(_buffer.get()), (size + 3) & ~3)) {
                return "flash read failed";
            }

            if (!offset) {
                _buffer[2] = _mode;
            }
            br_sha256_update(&_sha, _buffer.get(), size);
            yield();
        }

        uint8_t check[br_sha256_SIZE];
        br_sha256_out(&_sha, check);
        if (memcmp(digest, check, sizeof(digest))) {
            return "the file read back from flash differs, not booted";
        }

        eboot_command command {};
        command.action = ACTION_COPY_RAW;
        command.args[0] = _start;
        command.args[1] = 0;
        command.args[2] = _size;
        eboot_command_write(&command);
        _buffer.reset();
        return nullptr;
    }

    // nothing was told to eboot, the running image stays
    void abort() override {
        _buffer.reset();
    }

private:
    const char* flush() {
        const uint32_t address = _start + _written;
        if (!_written) {
            // a plain image gets the flash mode of the running one (as Updater); the hash is over the original byte
            const bool gzip = (_buffer[0] == 0x1f) && (_buffer[1] == 0x8b);
            _mode = _buffer[2];
            if (!gzip) {
                _buffer[2] = ESP.getFlashChipMode();
            }
        }

        memset(_buffer.get() + _len, 0xff, ((_len + 3) & ~3) - _len);
        const bool ok = !spi_flash_erase_sector(address / SPI_FLASH_SEC_SIZE)
            && !spi_flash_write(address, reinterpret_cast<uint32_t*>(_buffer.get()), (_len + 3) & ~3);
        _written += _len;
        _len = 0;
        return ok ? nullptr : "flash write failed";
    }

    std::unique_ptr<uint8_t[]> _buffer;
    br_sha256_context _sha;
    uint32_t _start { 0 };
    size_t _size { 0 };
    size_t _written { 0 };
    size_t _len { 0 };
    uint8_t _mode { 0 };
};

// The download (espurna/libs/http_client.h) into the writer. The client is asynchronous; the migration image does nothing else
// meanwhile, so it drives it here until it is done
bool download(const String& url, const String& sha256, String& result) {
    Writer writer;
    espurna::image::Receiver receiver(writer);
    espurna::http::Client client;
    bool finished { false };
    bool ok { false };
    client.start(url.c_str(), sha256.c_str(), receiver, [&](const espurna::http::Result& out) {
        finished = true;
        ok = out.ok;
        result = out.message;
    });

    while (!finished) {
        client.loop();
        yield();
    }

    return ok;
}

// -----------------------------------------------------------------------------
// Wi-Fi
// -----------------------------------------------------------------------------

bool ap_mode { false };
bool joined { false };
unsigned long wifi_since { 0 };

uint32_t ip(const String& text) {
    ip4_addr_t out;
    return ip4addr_aton(text.c_str(), &out) ? out.addr : 0;
}

String wifiText() {
    if (!ap_mode && (WiFi.status() != WL_CONNECTED)) {
        return "not connected";
    }

    String out(ap_mode ? "own access point " : "connected to ");
    char addr[16];
    ip4_addr_t value;
    if (ap_mode) {
        out += WiFi.softAPSSID();
        value.addr = WiFi.softAPIP();
    } else {
        out += WiFi.SSID();
        value.addr = WiFi.localIP();
    }

    out += ", ";
    out += ip4addr_ntoa_r(&value, addr, sizeof(addr));
    return out;
}

// ssid0 / ssid1 in turns (15 s each when both are set) until connected or `window` is over
bool join(unsigned long window) {
    ap_mode = false;
    joined = true;
    wifi_since = millis();
    WiFi.mode(WIFI_STA);

    const size_t networks = (config.ssid[0].length() ? 1 : 0) + (config.ssid[1].length() ? 1 : 0);
    if (!networks) {
        LOG("No Wi-Fi in the settings");
        return false;
    }

    WiFi.hostname(config.hostname);
    if (ip(config.ip) && ip(config.gw) && ip(config.mask)) {
        const auto dns = ip(config.dns);
        WiFi.config(ip(config.ip), ip(config.gw), ip(config.mask), dns ? dns : ip(config.gw));
    }

    const unsigned long slice = (networks > 1) ? 15000 : window;
    size_t index = config.ssid[0].length() ? 0 : 1;
    for (;;) {
        LOG("Connecting to %s", config.ssid[index].c_str());
        WiFi.begin(config.ssid[index], config.pass[index]);
        const auto attempt = millis();
        while (millis() - attempt < slice) {
            if (WiFi.status() == WL_CONNECTED) {
                LOG("Connected");
                return true;
            }

            if (millis() - wifi_since > window) {
                WiFi.disconnect();
                return false;
            }

            delay(100);
        }

        WiFi.disconnect();
        if (networks > 1) {
            index ^= 1;
        }
    }
}

void accessPoint() {
    // WPA2 needs 8..63 characters, ESPurna enforces the same for adminPass; SSID up to 32
    const String ssid = config.hostname.substring(0, 22) + "-MIGRATION";
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, (config.password.length() >= 8) ? config.password : String(DefaultPassword), 1, false, 2);
    ap_mode = true;
    wifi_since = millis();
    LOG("Access point %s", ssid.c_str());
}

// -----------------------------------------------------------------------------
// web
// -----------------------------------------------------------------------------

// The one HTTP server of ESPurna (espurna/libs/http_server.h): Digest as the ESPurna WebUI (realm = hostname); a
// nonce of another boot or of ESPurna itself is answered stale, the browser repeats with the new one without asking
using WebRequest = espurna::http::server::Request;
using WebMethod = espurna::http::server::Method;
espurna::http::server::Server server;
bool restart { false };
unsigned long last_activity { 0 };

bool authorized(WebRequest& request) {
    last_activity = millis();
    if (request.authenticate("admin", config.password.c_str(), config.hostname.c_str())) {
        return true;
    }

    request.requestAuthentication(config.hostname.c_str());
    return false;
}

void respond(WebRequest& request, int code, const char* type, const String& body) {
    request.addHeader("Cache-Control", "no-store");
    request.send(code, type, reinterpret_cast<const uint8_t*>(body.c_str()), body.length());
}

void text(WebRequest& request, int code, const String& body) {
    respond(request, code, "text/plain", body);
}


// also used inside <script>: the one escaping of ESPurna (espurna/libs/text.h)
void json(String& out, const String& value) {
    espurna::text::jsonString(out, value);
}

// the /ota/status of the full image (ota_fetch.cpp): fwinstall.mjs tells the images apart by "image"
String status() {
    String out("{\"image\":\"");
    out += Image;
    out += "\",\"fetch\":1,\"free\":";
    out += ESP.getFreeSketchSpace();
    out += ",\"url\":";
    json(out, storage.get(keys::Url));
    out += ",\"result\":";
    json(out, last_result);
    out += '}';
    return out;
}

// Page: the browser reads the release list (GitHub Pages sends Access-Control-Allow-Origin: *) and shows the 1 MB
// images that fit next to this one (Update for one byte more, in whole sectors); the board downloads the picked
// file itself after a reboot (/ota/fetch), or takes an uploaded file (/upgrade, the file as the body)
const char PageHead[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>ESPurna migration</title><style>body{font-family:sans-serif;max-width:640px;margin:16px auto;padding:0 16px}"
    "button{margin-top:8px}fieldset{margin-top:16px}</style></head><body><h2 id=h></h2><p id=st></p>"
    "<fieldset><legend>Releases</legend><div id=rel>Reading the release list...</div></fieldset>"
    "<fieldset><legend>Upload an image (.bin or .bin.gz)</legend><input type=file id=file accept=.bin,.gz><br>"
    "<button id=up>Upload and boot</button></fieldset><p id=log></p><script>";

const char PageScript[] PROGMEM =
    "const $=i=>document.getElementById(i),L=$('log'),I=S.image,F=S.free;"
    "$('h').textContent=document.title=H+' - ESPurna migration';"
    "$('st').textContent='Wi-Fi: '+W+'. Free: '+F+' bytes. Last update: '+(S.result||'none');"
    "function wait(){let away=0,t=0;const tick=async()=>{t+=3;"
    "try{const r=await fetch('/ota/status',{cache:'no-store'});"
    "const s=r.status===404?{}:await r.json();"
    "if(s.image!==I){L.textContent='ESPurna is back '+(s.result||'');setTimeout(()=>location.href='/',3000);return}"
    "if(away&&s.result){L.textContent=s.result;return}}catch(e){away=1}"
    "L.textContent='Waiting for the board ('+t+' s)...';setTimeout(tick,3000)};setTimeout(tick,3000)}"
    "function lock(v){for(const b of document.querySelectorAll('button'))b.disabled=v}"
    "async function post(u,b){lock(true);try{const r=await fetch(u,{method:'POST',body:b});"
    "L.textContent=await r.text();if(r.ok){wait();return}}catch(e){L.textContent=e}lock(false)}"
    "$('up').onclick=()=>{const f=$('file').files[0];if(!f){L.textContent='Pick a file';return}"
    "L.textContent='Uploading '+f.name+'...';post('/upgrade',f)};"
    "fetch(M,{cache:'no-store'}).then(r=>r.json()).then(m=>{const d=$('rel');d.textContent='';"
    "for(const r of m.releases||[]){const im=r.images||{};"
    "for(const n of Object.keys(im)){if(!n.startsWith('esp8266-1m')||n===I)continue;"
    "for(const f of im[n]){if(f.kind!=='ota'||!f.sha256||(f.size-f.size%4096+4096)>F)continue;"
    "const b=document.createElement('button');b.textContent=r.tag+': '+f.name+' ('+Math.round(f.size/1024)+' KB)';"
    "b.onclick=()=>{if(confirm('Install '+f.name+' of '+r.tag+'?')){L.textContent='Storing the request...';"
    "post('/ota/fetch',new URLSearchParams({url:new URL(f.url,M).href,sha256:f.sha256}))}};"
    "d.append(b,document.createElement('br'))}}}"
    "if(!d.childNodes.length)d.textContent='No 1 MB image that fits in '+F+' bytes in '+M})"
    ".catch(e=>$('rel').textContent='Cannot read '+M+': '+e)</script></body></html>";

void onRoot(WebRequest& request) {
    if (!authorized(request)) {
        return;
    }

    String body(FPSTR(PageHead));
    body += "const S=";
    body += status();
    body += ",H=";
    json(body, config.hostname);
    body += ",M=";
    json(body, config.manifest);
    body += ",W=";
    json(body, wifiText());
    body += ';';
    body += FPSTR(PageScript);
    respond(request, 200, "text/html", body);
}

void onStatus(WebRequest& request) {
    if (authorized(request)) {
        respond(request, 200, "application/json", status());
    }
}

// url + sha256 (a form, or the FormData of fwinstall.mjs), the same checks as the full image. Stored, then the reboot
// takes it (one attempt)
void onFetch(WebRequest& request) {
    if (!authorized(request)) {
        return;
    }

    // a page of another site must not make the board flash something (espurna/libs/image_receiver.h)
    if (!espurna::image::sameOrigin(request.header("Origin"), request.header("Host"),
            request.header("X-Forwarded-Host")))
    {
        text(request, 403, "ERROR: Origin");
        return;
    }

    const char* url_param = request.param("url");
    const char* sha_param = request.param("sha256");
    const String url(url_param ? url_param : "");
    const String sha256(sha_param ? sha_param : "");
    if (!validUrl(url) || !validSha256(sha256)) {
        text(request, 400, "ERROR: invalid URL or SHA-256");
        return;
    }

    const Pair pairs[] {
        {keys::Url, url},
        {keys::Sha256, sha256},
    };
    if (!storage.write(pairs, 2, keys::Result)) {
        text(request, 500, "ERROR: cannot write the settings");
        return;
    }

    last_result = String();
    text(request, 200, "OK, fetch");
    restart = true;
}

// .bin or .bin.gz: the file as the body (page of this image), or the file of a form as ESPurna's /upgrade; the one
// image receiver into the writer
struct Upload {
    Upload() :
        receiver(writer)
    {}

    Writer writer;
    espurna::image::Receiver receiver;
    const char* refused { nullptr };
};

// a page of another site must not make the board flash something (espurna/libs/image_receiver.h sameOrigin())
bool sameOrigin(WebRequest& request) {
    return espurna::image::sameOrigin(
        request.header("Origin"), request.header("Host"), request.header("X-Forwarded-Host"));
}

void onUpgradeBody(WebRequest& request, const char*, size_t index, const uint8_t* data, size_t len, bool) {
    auto* upload = static_cast<Upload*>(request.context());
    if (!index) {
        if (!request.authenticate("admin", config.password.c_str(), config.hostname.c_str())) {
            return;
        }

        upload = new (std::nothrow) Upload();
        if (!upload) {
            return;
        }
        request.setContext(upload, [](void* ptr) {
            delete static_cast<Upload*>(ptr);
        });

        if (!sameOrigin(request)) {
            upload->refused = "Origin";
            return;
        }

        // placed at the end of the free space: its size first
        LOG("Upload of %d bytes", request.fileSize());
        upload->receiver.expect(request.fileSize());
    }

    if (!upload || upload->refused) {
        return;
    }

    upload->receiver.push(data, len);
}

void onUpgrade(WebRequest& request) {
    if (!authorized(request)) {
        return;
    }

    auto* upload = static_cast<Upload*>(request.context());
    const char* error = !upload ? "invalid request" : upload->refused ? upload->refused : upload->receiver.finish();
    if (error) {
        LOG("Upload failed: %s", error);
        text(request, (upload && upload->refused) ? 403 : 400, String("ERROR: ") + error);
        return;
    }

    // the full image shows it in /ota/status instead of an earlier failure
    storeResult(String("ok: uploaded ") + upload->receiver.received() + " bytes");
    text(request, 200, "OK");
    restart = true;
}

// The request of the full image, taken once: erased before anything else (a power cut or a crash during the
// download leaves the provisional result), then Wi-Fi and the download
void takeRequest() {
    const auto url = storage.get(keys::Url);
    if (!url.length()) {
        return;
    }

    const auto sha256 = storage.get(keys::Sha256);
    LOG("Request %s", url.c_str());

    const Pair pair { keys::Result, "failed: the migration image restarted during the download (power cut or crash)" };
    if (!storage.write(&pair, 1, keys::Url, keys::Sha256)) {
        // never retried over the next boots
        last_result = "failed: the request could not be erased from the settings";
        return;
    }

    String message;
    bool ok { false };
    if (!validUrl(url) || !validSha256(sha256)) {
        message = "invalid URL or SHA-256 in the request";
    } else if (!join(RequestConnectWindow)) {
        message = "no Wi-Fi connection";
    } else {
        ok = download(url, sha256, message);
    }

    storeResult(String(ok ? "ok: " : "failed: ") + message);
    if (ok) {
        delay(100);
        ESP.restart();
    }
}

} // namespace

void setup() {
#if MIGRATION_LOG
    USD(0) = ESP8266_CLOCK / 115200;
#endif
    LOG("ESPurna migration, built " __DATE__ " " __TIME__ ", %d bytes free next to it", ESP.getFreeSketchSpace());

    storage.load();
    config.ssid[0] = storage.get("ssid0");
    config.pass[0] = storage.get("pass0");
    config.ssid[1] = storage.get("ssid1");
    config.pass[1] = storage.get("pass1");
    config.ip = storage.get("ip0");
    config.gw = storage.get("gw0");
    config.mask = storage.get("mask0");
    config.dns = storage.get("dns0");
    config.hostname = storage.get("hostname");
    config.password = storage.get("adminPass");
    config.manifest = storage.get("webFwUrl");
    last_result = storage.get(keys::Result);

    if (!config.hostname.length()) {
        // ESPURNA-<the last 3 bytes of the MAC>, as ESPurna (arch/esp8266/system_esp8266.cpp)
        const uint32_t reg0 = READ_PERI_REG(0x3ff00050);
        const uint32_t reg1 = READ_PERI_REG(0x3ff00054);
        const uint8_t id[3] { static_cast<uint8_t>(reg1 >> 8), static_cast<uint8_t>(reg1),
            static_cast<uint8_t>(reg0 >> 24) };
        config.hostname = String("ESPURNA-") + hex(id, sizeof(id));
        config.hostname.toUpperCase();
    }

    if (!config.password.length()) {
        config.password = DefaultPassword;
    }

    if (!config.manifest.length()) {
        config.manifest = DefaultManifest;
    }

    WiFi.persistent(false);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);

    takeRequest();

    // not twice: a request that found no Wi-Fi waited 3 minutes already
    if ((WiFi.status() != WL_CONNECTED) && (joined || !join(ConnectWindow))) {
        accessPoint();
    }

    server.on("/", WebMethod::Get, onRoot);
    server.on("/ota/status", WebMethod::Get, onStatus);
    server.on("/ota/fetch", WebMethod::Post, onFetch);
    server.on("/upgrade", WebMethod::Post, onUpgrade, onUpgradeBody);
    server.begin(80);
    last_activity = millis();
    LOG("Web page on port 80, heap %d", ESP.getFreeHeap());
}

void loop() {
    server.loop();

    // the answer goes out first: the server runs a little longer
    if (restart) {
        const auto start = millis();
        while (server.active() && (millis() - start < 500)) {
            server.loop();
            yield();
        }
        ESP.restart();
    }

    const auto now = millis();
    if (ap_mode) {
        // a router back after a power cut: the Wi-Fi again, unless somebody uses the access point
        if ((now - wifi_since > FallbackAfter) && (now - last_activity > FallbackAfter)
            && !WiFi.softAPgetStationNum() && !join(ConnectWindow))
        {
            accessPoint();
        }
    } else if (WiFi.status() == WL_CONNECTED) {
        wifi_since = now;
    } else if (now - wifi_since > FallbackAfter) {
        accessPoint();
    }
}
