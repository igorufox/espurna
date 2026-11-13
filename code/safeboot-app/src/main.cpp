/*

ESPurna safeboot (ESP32, ESP-IDF)

Small recovery / update image for the factory partition of esp32_4mb_safeboot.csv (0x10000, 832 KB). ESPurna lives in
the single big OTA slot (app0) and cannot overwrite itself, so updates go through this image:
- Wi-Fi (ssid0/pass0, ssid1/pass1, static ip0/gw0/mask0/dns0), hostname and adminPass are read straight from the
  ESPurna settings: the settings pool of ESPurna (espurna/libs/settings_rotate.h: the top 8 sectors of the "spiffs"
  data partition, arch/esp32/storage_flash_esp32.cpp), or the NVS blob "eeprom"/"eeprom" ESPurna had before and takes
  over on its next boot; Embedis key-value format written right to left; no settings of its own
- web UI on port 80, user admin + the ESPurna admin password: pick a release (the list comes from firmware.json on
  GitHub Pages, the browser downloads the file and uploads it here), upload an image into app0, boot ESPurna. The
  board downloads nothing itself: every image comes from the browser, as in ESPurna's WebUI (no update by an URL)
- no Wi-Fi in 30 s: own access point <hostname>-SAFEBOOT (WPA2, admin password) at 192.168.4.1
- the next boot is set back to app0 right at start (when it holds a valid image): a crash or a power cut here always
  returns to ESPurna; nothing done for 15 minutes: back to ESPurna as well

*/

#include <esp_event.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/inet.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// shared with ESPurna and code/migration-app: the settings records, the partition table
#include "../../espurna/libs/embedis_records.h"
#include "../../espurna/libs/settings_rotate.h"
#include "../../espurna/libs/esp32_partitions.h"
#include "../../espurna/libs/text.h"

// the one network stack of ESPurna (libs/net_impl.h: TCP and its HTTP server; no client here). lwIP's callbacks run in
// the tcpip thread, the server in this task
namespace safeboot {

struct Lock {
    Lock() :
        handle(xSemaphoreCreateMutex())
    {}

    ~Lock() {
        vSemaphoreDelete(handle);
    }

    void lock() {
        xSemaphoreTake(handle, portMAX_DELAY);
    }

    void unlock() {
        xSemaphoreGive(handle);
    }

    SemaphoreHandle_t handle;
};

} // namespace safeboot

#define HTTP_CLIENT_AGENT "ESPurna-safeboot"
#define NET_LOG(...) do {} while (false)
#define NET_LOCK ::safeboot::Lock
#include "../../espurna/libs/net_impl.h"
#include "../../espurna/libs/image_receiver.h"

namespace espurna {
namespace tls {

// the hooks of the one TLS (libs/tls.h): nothing connects out of safeboot
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

constexpr char DefaultPassword[] = "fibonacci";
constexpr char DefaultManifest[] = "https://igorufox.github.io/espurna/firmware.json";
constexpr int ConnectTimeoutMs { 30000 };
constexpr int64_t IdleTimeoutUs { 15LL * 60 * 1000 * 1000 };

// --- ESPurna settings ------------------------------------------------------------------------------------------

struct Settings {
    std::string ssid[2];
    std::string pass[2];
    std::string ip;
    std::string gw;
    std::string mask;
    std::string dns;
    std::string hostname;
    std::string password;
    std::string manifest;
};

Settings settings;

// the ESPurna settings blob: Embedis records from its end towards its beginning (espurna/libs/embedis_records.h)
struct Embedis {
    const uint8_t* data;
    size_t size;

    std::string get(const char* key) const {
        espurna::embedis_records::Record record;
        if (!espurna::embedis_records::find(data, 0, size, key, record)) {
            return std::string();
        }

        return std::string(reinterpret_cast<const char*>(data + record.value), record.value_len);
    }
};

// the newest valid copy of ESPurna's pool: the top 8 sectors of the "spiffs" data partition
bool readPool(uint8_t* data) {
    constexpr size_t Sectors { 8 };
    constexpr size_t Size { espurna::settings_rotate::SectorSize };

    const auto* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (!part || (part->size < (Sectors * Size))) {
        return false;
    }

    const auto found = espurna::settings_rotate::find(Sectors, data,
        [&](size_t index, uint8_t* out) {
            return esp_partition_read(part, part->size - ((index + 1) * Size), out, Size) == ESP_OK;
        });

    return found.valid;
}

// the NVS blob ESPurna had before the pool (the end of its buffer)
size_t readBlob(std::unique_ptr<uint8_t[]>& out) {
    size_t size = 0;
    nvs_handle_t handle;
    if (nvs_open("eeprom", NVS_READONLY, &handle) == ESP_OK) {
        if ((nvs_get_blob(handle, "eeprom", nullptr, &size) == ESP_OK) && size) {
            out.reset(new (std::nothrow) uint8_t[size]);
            if (!out || (nvs_get_blob(handle, "eeprom", out.get(), &size) != ESP_OK)) {
                out.reset();
                size = 0;
            }
        }
        nvs_close(handle);
    }

    return out ? size : 0;
}

void readSettings() {
    std::unique_ptr<uint8_t[]> data(new (std::nothrow) uint8_t[espurna::settings_rotate::SectorSize]);
    size_t size = 0;
    if (data && readPool(data.get())) {
        size = espurna::settings_rotate::SectorSize;
    } else {
        size = readBlob(data);
    }

    if (size) {
        const Embedis kv{data.get(), size};
        settings.ssid[0] = kv.get("ssid0");
        settings.pass[0] = kv.get("pass0");
        settings.ssid[1] = kv.get("ssid1");
        settings.pass[1] = kv.get("pass1");
        settings.ip = kv.get("ip0");
        settings.gw = kv.get("gw0");
        settings.mask = kv.get("mask0");
        settings.dns = kv.get("dns0");
        settings.hostname = kv.get("hostname");
        settings.password = kv.get("adminPass");
        settings.manifest = kv.get("webFwUrl");
    }

    if (settings.hostname.empty()) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char out[24];
        snprintf(out, sizeof(out), "ESPURNA-%02X%02X%02X", mac[3], mac[4], mac[5]);
        settings.hostname = out;
    }

    if (settings.password.empty()) {
        settings.password = DefaultPassword;
    }

    if (settings.manifest.empty()) {
        settings.manifest = DefaultManifest;
    }
}

std::string nvsString(nvs_handle_t handle, const char* key) {
    std::string out;

    size_t size = 0;
    if ((nvs_get_str(handle, key, nullptr, &size) == ESP_OK) && (size > 1)) {
        std::unique_ptr<char[]> value(new (std::nothrow) char[size]);
        if (value && (nvs_get_str(handle, key, value.get(), &size) == ESP_OK)) {
            out = value.get();
        }
    }

    return out;
}

// the image ESPurna in app0 was built as (ESPURNA_IMAGE, written by ESPurna): the release list shows its files
std::string runningImage() {
    std::string out;

    nvs_handle_t handle;
    if (nvs_open("safeboot", NVS_READONLY, &handle) == ESP_OK) {
        out = nvsString(handle, "image");
        nvs_close(handle);
    }

    return out;
}

// --- app0 -------------------------------------------------------------------------------------------------------

const esp_partition_t* app() {
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
}

// ESPurna writes NVS "safeboot"/"app" at boot: the app description of an Arduino build names the prebuilt IDF
// libraries ("arduino-lib-builder"), not ESPurna. Erased whenever safeboot writes app0, until ESPurna boots again
std::string appInfo() {
    esp_app_desc_t desc;
    const auto* part = app();
    if (!part || (esp_ota_get_partition_description(part, &desc) != ESP_OK)) {
        return "no valid image";
    }

    std::string written;
    nvs_handle_t handle;
    if (nvs_open("safeboot", NVS_READONLY, &handle) == ESP_OK) {
        written = nvsString(handle, "app");
        nvs_close(handle);
    }
    if (!written.empty()) {
        return written;
    }

    return std::string(desc.project_name) + " " + desc.version + ", built " + desc.date + " " + desc.time;
}

void forgetAppInfo() {
    nvs_handle_t handle;
    if (nvs_open("safeboot", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, "app");
        nvs_commit(handle);
        nvs_close(handle);
    }
}

bool bootApp(std::string& error) {
    esp_app_desc_t desc;
    const auto* part = app();
    if (!part || (esp_ota_get_partition_description(part, &desc) != ESP_OK)) {
        error = "no valid image in app0";
        return false;
    }

    const auto result = esp_ota_set_boot_partition(part);
    if (result != ESP_OK) {
        error = esp_err_to_name(result);
        return false;
    }

    return true;
}

// One image write into app0, from the application image (0xE9 magic byte first) or from the merged image of a release
// (the whole flash from 0x0: bootloader, partition table at 0x8000, safeboot, ESPurna): of that one only the part at
// the offset of ota_0 in its own partition table goes into app0. end() verifies the image
constexpr size_t MergedTableOffset { espurna::esp32_partitions::TableOffset };
constexpr size_t MergedTableSize { espurna::esp32_partitions::TableSize };

struct Writer final : public espurna::image::Target {
    esp_ota_handle_t handle { 0 };
    const esp_partition_t* part { nullptr };
    size_t written { 0 };    // into app0
    size_t received { 0 };   // of the file
    size_t expected { 0 };   // Content-Length of the file, 0 - not known (app0 is erased whole then)
    size_t app_offset { 0 }; // where the application starts in the file
    bool merged { false };
    bool located { false };
    std::vector<uint8_t> head; // merged image, up to the end of its partition table
    std::string error;

    Writer() = default;
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    // ota_0 entry of the partition table in the merged image (espurna/libs/esp32_partitions.h)
    bool locate() {
        espurna::esp32_partitions::Entry entry;
        if (espurna::esp32_partitions::find(head.data() + MergedTableOffset, MergedTableSize,
                ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, entry))
        {
            app_offset = entry.offset;
            located = true;
            return true;
        }

        error = "a merged image without an ota_0 application in its partition table";
        return false;
    }

    bool begin(const char* data, size_t len) {
        if (!len || (static_cast<uint8_t>(data[0]) != 0xE9)) {
            error = merged
                ? "no ESP32 application at the ota_0 offset of the merged image"
                : "not an ESP32 image (no 0xE9 magic byte, no merged image)";
            return false;
        }

        const size_t size = (expected > app_offset) ? (expected - app_offset) : 0;
        part = app();
        if (part && size && (size > part->size)) {
            error = "the image is bigger than app0";
            part = nullptr;
            return false;
        }

        // erases only what the image takes when its size is known
        const auto result = part
            ? esp_ota_begin(part, size ? size : OTA_SIZE_UNKNOWN, &handle) : ESP_ERR_NOT_FOUND;
        if (result != ESP_OK) {
            error = std::string("begin: ") + esp_err_to_name(result);
            part = nullptr;
            return false;
        }

        return true;
    }

    bool writeApp(const char* data, size_t len) {
        if (!len) {
            return true;
        }

        if (!part && !begin(data, len)) {
            return false;
        }

        const auto result = esp_ota_write(handle, data, len);
        if (result != ESP_OK) {
            error = std::string("write: ") + esp_err_to_name(result);
            return false;
        }

        written += len;
        return true;
    }

    bool write(const char* data, size_t len) {
        if (!len) {
            return true;
        }

        // the first byte decides: an application image, or a merged one starting with the erased area before the
        // bootloader (0xFF up to 0x1000)
        if (!received && !part) {
            merged = static_cast<uint8_t>(data[0]) == 0xFF;
        }

        const size_t at = received;
        received += len;

        if (!merged) {
            return writeApp(data, len);
        }

        if (!located) {
            const size_t want = MergedTableOffset + MergedTableSize;
            const size_t take = std::min(len, want - head.size());
            head.insert(head.end(), data, data + take);
            if (head.size() < want) {
                return true;
            }
            if (!locate()) {
                return false;
            }
            head.clear();
            head.shrink_to_fit();
        }

        // the part of this chunk at or after the application offset
        const size_t end = at + len;
        if (end <= app_offset) {
            return true;
        }

        const size_t skip = (at < app_offset) ? (app_offset - at) : 0;
        return writeApp(data + skip, len - skip);
    }

    bool finish() {
        if (!part) {
            if (error.empty()) {
                error = "no data";
            }
            return false;
        }

        auto result = esp_ota_end(handle);
        part = nullptr;
        if (result == ESP_OK) {
            forgetAppInfo();
            result = esp_ota_set_boot_partition(app());
        }

        if (result != ESP_OK) {
            error = std::string("image check failed: ") + esp_err_to_name(result);
            return false;
        }

        return true;
    }

    void abort() override {
        if (part) {
            esp_ota_abort(handle);
            part = nullptr;
        }
    }

    // --- the target of the image receiver (espurna/libs/image_receiver.h): the size is known (Content-Length of the
    // upload) or 0, the first byte is checked by write() (an application, or a merged image)
    const char* begin(const uint8_t* data, size_t len, size_t size) override {
        if (!len || ((data[0] != 0xE9) && (data[0] != 0xFF))) {
            return "not an ESP32 image (no 0xE9 magic byte, no merged image)";
        }

        expected = size;
        received = 0;
        written = 0;
        merged = false;
        located = false;
        head.clear();
        error.clear();
        return nullptr;
    }

    const char* write(const uint8_t* data, size_t len) override {
        return write(reinterpret_cast<const char*>(data), len) ? nullptr : error.c_str();
    }

    const char* end(size_t) override {
        return finish() ? nullptr : error.c_str();
    }
};

std::string last_result;

// --- Wi-Fi ------------------------------------------------------------------------------------------------------

EventGroupHandle_t events;
constexpr EventBits_t Connected = BIT0;
esp_netif_t* netif_sta { nullptr };
std::string wifi_result;

void onWifi(void*, esp_event_base_t base, int32_t id, void* data) {
    if ((base == WIFI_EVENT) && (id == WIFI_EVENT_STA_DISCONNECTED)) {
        xEventGroupClearBits(events, Connected);
        const auto* event = static_cast<const wifi_event_sta_disconnected_t*>(data);
        wifi_result = "disconnected, reason " + std::to_string(event->reason);
    } else if ((base == IP_EVENT) && (id == IP_EVENT_STA_GOT_IP)) {
        xEventGroupSetBits(events, Connected);
    }
}

bool connect() {
    netif_sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif_sta, settings.hostname.c_str());

    if (!settings.ip.empty()) {
        esp_netif_ip_info_t info {};
        if ((esp_netif_str_to_ip4(settings.ip.c_str(), &info.ip) == ESP_OK)
            && (esp_netif_str_to_ip4(settings.gw.c_str(), &info.gw) == ESP_OK)
            && (esp_netif_str_to_ip4(settings.mask.c_str(), &info.netmask) == ESP_OK))
        {
            esp_netif_dhcpc_stop(netif_sta);
            esp_netif_set_ip_info(netif_sta, &info);
            esp_netif_dns_info_t dns {};
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            if (esp_netif_str_to_ip4(settings.dns.c_str(), &dns.ip.u_addr.ip4) != ESP_OK) {
                dns.ip.u_addr.ip4.addr = info.gw.addr;
            }
            esp_netif_set_dns_info(netif_sta, ESP_NETIF_DNS_MAIN, &dns);
        }
    }

    // same as ESPurna (arch/esp32/wifi_driver_esp32.cpp): EU, channels 1..13 scanned actively. The default "01" leaves 12 and 13
    // out, the APs here are on 13
    const wifi_country_t country { "EU", 1, 13, 20, WIFI_COUNTRY_POLICY_AUTO };
    esp_wifi_set_country(&country);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    for (size_t index = 0; index < 2; ++index) {
        if (settings.ssid[index].empty()) {
            continue;
        }

        wifi_config_t config {};
        strncpy(reinterpret_cast<char*>(config.sta.ssid), settings.ssid[index].c_str(), sizeof(config.sta.ssid));
        strncpy(reinterpret_cast<char*>(config.sta.password), settings.pass[index].c_str(),
            sizeof(config.sta.password));
        config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        config.sta.pmf_cfg.capable = true;
        config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        esp_wifi_set_config(WIFI_IF_STA, &config);
        esp_wifi_connect();

        const auto bits = xEventGroupWaitBits(events, Connected, pdFALSE, pdTRUE,
            pdMS_TO_TICKS(ConnectTimeoutMs));
        if (bits & Connected) {
            wifi_result = "connected to " + settings.ssid[index];
            return true;
        }

        esp_wifi_disconnect();
    }

    return false;
}

void accessPoint() {
    esp_netif_create_default_wifi_ap();

    wifi_config_t config {};
    const std::string ssid = settings.hostname + "-SAFEBOOT";
    strncpy(reinterpret_cast<char*>(config.ap.ssid), ssid.c_str(), sizeof(config.ap.ssid));
    config.ap.ssid_len = ssid.size();
    // WPA2 needs 8..63 characters, ESPurna enforces the same for adminPass
    const auto& password = (settings.password.size() >= 8) ? settings.password : std::string(DefaultPassword);
    strncpy(reinterpret_cast<char*>(config.ap.password), password.c_str(), sizeof(config.ap.password));
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection = 2;
    config.ap.channel = 1;

    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &config);
    esp_wifi_start();
}

// --- web --------------------------------------------------------------------------------------------------------

// The one HTTP server of ESPurna (espurna/libs/http_server.h), driven by app_main's loop
using WebRequest = espurna::http::server::Request;
espurna::http::server::Server server;

int64_t last_activity { 0 };
bool restart { false };

// The login of every ESPurna page (the server's Digest, as the head and the migration image), realm "<hostname> safeboot": the
// /safeboot page of ESPurna tells safeboot from the head by that realm
bool authenticated(WebRequest& request) {
    last_activity = esp_timer_get_time();

    const std::string realm = settings.hostname + " safeboot";
    if (request.authenticate("admin", settings.password.c_str(), realm.c_str())) {
        return true;
    }

    request.requestAuthentication(realm.c_str());
    return false;
}

void send(WebRequest& request, const std::string& body, int code = 200) {
    std::string out;
    out.reserve(2048 + body.size());
    out += "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" "
           "content=\"width=device-width,initial-scale=1\"><title>";
    out += settings.hostname;
    out += " safeboot</title><style>body{font-family:sans-serif;max-width:640px;margin:16px auto;padding:0 16px}"
           "button{margin-top:8px;padding:6px 16px}input[type=text]{width:100%;box-sizing:border-box}"
           "fieldset{margin-top:16px}#log{margin-top:12px}</style></head><body><h2>";
    out += settings.hostname;
    out += " - safeboot</h2>";
    out += body;
    out += "</body></html>";

    request.send(code, "text/html", reinterpret_cast<const uint8_t*>(out.data()), out.size());
}

// a JS string literal, safe inside <script> (the one escaping of ESPurna, libs/text.h)
std::string jsString(const std::string& value) {
    std::string out;
    espurna::text::jsonString(out, value);
    return out;
}

// The image goes up as the raw request body (no multipart), the page script sends the picked file with fetch().
// Releases: the browser reads firmware.json (GitHub Pages sends it with Access-Control-Allow-Origin: *) and lists the
// update files ("ota": the merged image of a release, -ota.bin of the older ones) of the ESPurna image in app0 (all
// ESP32 images when it is not known); the browser downloads the picked one and uploads it (/upload), as the button
// below does with a file (GitHub Pages sends the files with Access-Control-Allow-Origin: * as well)
constexpr char PageScript[] =
    "const $=i=>document.getElementById(i);"
    // the answers are whole pages: their first paragraph is the result
    "async function msg(r){const d=new DOMParser().parseFromString(await r.text(),'text/html');"
    "const p=d.querySelector('p');return(p||d.body).textContent.trim()}"
    "$('up').onclick=async()=>{const f=$('file').files[0];"
    "const l=$('log');if(!f){l.textContent='Pick a file';return}l.textContent='Uploading...';"
    "try{const r=await fetch('/upload',{method:'POST',body:f});l.textContent=await msg(r);"
    "if(r.ok)setTimeout(()=>location.href='/',30000)}catch(e){l.textContent=e}};"
    "async function get(u){const l=$('rlog');l.textContent='Downloading '+u+'...';"
    "for(const b of document.querySelectorAll('#rel button'))b.disabled=true;"
    "try{const d=await fetch(u,{cache:'no-store'});if(!d.ok)throw'HTTP '+d.status+' for '+u;"
    "const body=await d.blob();l.textContent='Uploading '+Math.round(body.size/1024)+' KB...';"
    "const r=await fetch('/upload',{method:'POST',body});"
    "l.textContent=await msg(r);if(r.ok)setTimeout(()=>location.href='/',30000)}"
    "catch(e){l.textContent=e}}"
    "fetch(M,{cache:'no-store'}).then(r=>r.json()).then(m=>{const d=$('rel');d.textContent='';"
    "for(const r of m.releases||[]){const im=r.images||{};"
    "for(const n of Object.keys(im)){if(!n.startsWith('esp32')||(I&&n!==I))continue;"
    "for(const f of im[n]){if(f.kind!=='ota')continue;const b=document.createElement('button');"
    "b.textContent=r.tag+': '+f.name+' ('+Math.round(f.size/1024)+' KB)';"
    "b.onclick=()=>{if(confirm('Install '+f.name+' of '+r.tag+'?'))get(new URL(f.url,M).href)};"
    "d.append(b,document.createElement('br'))}}}"
    "if(!d.childNodes.length)d.textContent='No ESP32 image'+(I?' '+I:'')+' in '+M})"
    ".catch(e=>$('rel').textContent='Cannot read '+M+': '+e);";

void onRoot(WebRequest& request) {
    if (!authenticated(request)) {
        return;
    }

    const auto image = runningImage();
    std::string body = "<p>ESPurna in app0: " + appInfo() + "</p><p>Wi-Fi: " + wifi_result + "</p>";
    if (!last_result.empty()) {
        body += "<p>Last update: " + last_result + "</p>";
    }
    body += "<fieldset><legend>Releases" + (image.empty() ? std::string() : (" of " + image))
          + "</legend><div id=\"rel\">Reading the release list...</div><div id=\"rlog\"></div></fieldset>"
            "<fieldset><legend>Upload an image</legend><input type=\"file\" id=\"file\" accept=\".bin\"><br>"
            "<button id=\"up\">Upload and boot</button><div id=\"log\"></div></fieldset>"
            "<form method=\"post\" action=\"/boot\"><button>Boot ESPurna</button></form>";
    body += "<script>const M=" + jsString(settings.manifest) + ",I=" + jsString(image) + ";";
    body += PageScript;
    body += "</script>";
    send(request, body);
}

// a page of another site must not make the board flash or boot something (espurna/libs/image_receiver.h)
bool sameOrigin(WebRequest& request) {
    if (espurna::image::sameOrigin(request.header("Origin"), request.header("Host"),
            request.header("X-Forwarded-Host")))
    {
        return true;
    }

    request.send(403, "text/plain", "ERROR: Origin");
    return false;
}

void onBoot(WebRequest& request) {
    if (!authenticated(request) || !sameOrigin(request)) {
        return;
    }

    std::string error;
    if (!bootApp(error)) {
        send(request, "<p>" + error + "</p><p><a href=\"/\">Back</a></p>", 400);
        return;
    }

    send(request, "<p>Booting ESPurna</p>");
    restart = true;
}

// the image of an upload, the request's context: the one image receiver into the writer
struct Upload {
    Upload() :
        receiver(writer)
    {}

    Writer writer;
    espurna::image::Receiver receiver;
    bool refused { false };
};

void onUploadBody(WebRequest& request, const char*, size_t index, const uint8_t* data, size_t len, bool) {
    auto* upload = static_cast<Upload*>(request.context());
    if (!index) {
        if (!authenticated(request)) {
            return;
        }

        upload = new (std::nothrow) Upload();
        if (!upload) {
            return;
        }
        request.setContext(upload, [](void* ptr) {
            delete static_cast<Upload*>(ptr);
        });

        if (!espurna::image::sameOrigin(request.header("Origin"), request.header("Host"),
                request.header("X-Forwarded-Host")))
        {
            upload->refused = true;
            return;
        }

        upload->receiver.expect(request.fileSize());
    }

    if (!upload || upload->refused) {
        return;
    }

    upload->receiver.push(data, len);
    last_activity = esp_timer_get_time();
}

void onUpload(WebRequest& request) {
    if (!authenticated(request)) {
        return;
    }

    auto* upload = static_cast<Upload*>(request.context());
    if (upload && upload->refused) {
        request.send(403, "text/plain", "ERROR: Origin");
        return;
    }

    const char* error = upload ? upload->receiver.finish() : "no image";
    last_result = !error ? ("uploaded " + std::to_string(upload->writer.written) + " bytes") : std::string(error);
    const std::string text = !error ? (last_result + ", booting ESPurna") : last_result;
    request.send(!error ? 200 : 400, "text/plain", reinterpret_cast<const uint8_t*>(text.data()), text.size());
    restart = !error;
}

void web() {
    server.on("/", espurna::http::server::Method::Get, onRoot);
    server.on("/boot", espurna::http::server::Method::Post, onBoot);
    server.on("/upload", espurna::http::server::Method::Post, onUpload, onUploadBody);
    server.begin(80);
}

} // namespace

extern "C" void app_main() {
    // Next boot goes back to ESPurna whenever app0 holds a valid image: a crash, a power cut, a reset or a broken NVS
    // here must never leave the board in safeboot for good. Before anything else: the boot partition lives in otadata,
    // not in NVS. A finished upload sets app0 again anyway
    {
        std::string error;
        bootApp(error);
    }

    // never erase here: the partition holds the ESPurna settings. Without NVS: no settings, the access point
    const auto nvs = nvs_flash_init();
    const bool nvs_ok = (nvs == ESP_OK);

    if (nvs_ok) {
        readSettings();
    }

    events = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onWifi, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onWifi, nullptr);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (!nvs_ok) {
        init.nvs_enable = 0;
    }
    esp_wifi_init(&init);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    if (!nvs_ok || !connect()) {
        accessPoint();
    }

    web();
    last_activity = esp_timer_get_time();

    for (;;) {
        server.loop();
        vTaskDelay(pdMS_TO_TICKS(2));

        // the answer goes out first: the server runs a little longer
        if (restart) {
            const auto until = esp_timer_get_time() + 500 * 1000;
            while ((esp_timer_get_time() < until) && server.active()) {
                server.loop();
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            esp_restart();
        }

        // forgotten in safeboot, or here after a crash loop of ESPurna: give ESPurna another chance
        if ((esp_timer_get_time() - last_activity) > IdleTimeoutUs) {
            std::string error;
            if (bootApp(error)) {
                esp_restart();
            }
            last_activity = esp_timer_get_time();
        }
    }
}
