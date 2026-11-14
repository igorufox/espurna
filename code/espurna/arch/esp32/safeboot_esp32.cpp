/*

SAFEBOOT (ESP32)

ref. safeboot_esp32.h

*/

#include "espurna.h"

#if SAFEBOOT_SUPPORT

#include "safeboot_esp32.h"
#include "terminal.h"
#include "web.h"
#include "../../libs/esp32_partitions.h"
#include "../../libs/http_client.h"
#include "../../libs/image_receiver.h"
#include "../../libs/boot_trace.h"

#include <esp_attr.h>
#include <esp_flash.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <nvs.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace espurna {
namespace safeboot {
namespace {

constexpr uint32_t Magic { 0x53414642 }; // SAFB
constexpr uint32_t CrashLoopMax { SAFEBOOT_CRASH_LOOP_MAX };
constexpr uint32_t CrashLoopUptimeMs { 60000 };

RTC_NOINIT_ATTR uint32_t rtc_magic;
RTC_NOINIT_ATTR uint32_t rtc_count;

uint32_t count_at_boot { 0 };
bool cleared { false };

// Crashes count (guardEarly(), before any static initializer of ESPurna), and software restarts that ESPurna did not
// request itself (guard(), after rtcmem: ESPurna always stores a custom reason before its own restarts)
bool crashed(esp_reset_reason_t reason) {
    switch (reason) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_UNKNOWN:
        return true;
    default:
        break;
    }

    return false;
}

const esp_partition_t* factory() {
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
}

// esp_ota_get_next_update_partition() falls back to the first OTA slot, which is the running
// one when the table has a single OTA app. Update would then erase the running image
bool otaPossible() {
    const auto* running = esp_ota_get_running_partition();
    const auto* next = esp_ota_get_next_update_partition(nullptr);
    return next && running && (next->address != running->address);
}

bool switchToSafeboot(String& error) {
    const auto* part = factory();
    if (!part) {
        error = F("no factory (safeboot) app partition");
        return false;
    }

    const auto* running = esp_ota_get_running_partition();
    if (running && (running->address == part->address)) {
        error = F("already running from the factory partition");
        return false;
    }

    // Only into our own safeboot: it joins Wi-Fi with the ESPurna settings and points the next boot back to app0.
    // Anything else there may never come back (Tasmota's safeboot without its own settings boots with Wi-Fi off and
    // stays there: 10.0.5.66, 2026-11-05). An erased partition (merged image without safeboot) is refused as well
    esp_app_desc_t desc;
    if ((esp_ota_get_partition_description(part, &desc) != ESP_OK)
        || (std::strcmp(desc.project_name, "espurna-safeboot") != 0))
    {
        error = F("the factory partition does not hold the ESPurna safeboot image, install it first (/safeboot)");
        return false;
    }

    const auto result = esp_ota_set_boot_partition(part);
    if (result != ESP_OK) {
        error = esp_err_to_name(result);
        return false;
    }

    return true;
}

// --- rollback by the bootloader ----------------------------------------------------------------------------------
// A new image in app0 (an upload to safeboot, its "Boot ESPurna") is written as "new" (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE,
// the Arduino bootloader and safeboot-app). The bootloader starts it once as "pending verify"; a boot before ESPurna
// confirmed it (a crash anywhere, from the first static initializer on, a hang, a reset) makes the bootloader mark it
// aborted and start the factory partition: safeboot, on Wi-Fi with the same settings. No code of ESPurna has to run
// for that (W31: 10.0.5.66 looped and was lost twice on 11-13, the guard in ESPurna itself never ran)
// Confirmed: after ConfirmUptimeMs with the station connected, or when ESPurna restarts by itself (its loop runs).
// Not connected after ConfirmTimeoutMs: restart, back to safeboot (wrong Wi-Fi settings in a new image)
constexpr uint32_t ConfirmUptimeMs { 60000 };
constexpr uint32_t ConfirmTimeoutMs { 600000 };

bool pending_at_boot { false };

bool pending() {
    const auto* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    return running
        && (esp_ota_get_state_partition(running, &state) == ESP_OK)
        && (state == ESP_OTA_IMG_PENDING_VERIFY);
}

void confirm() {
    if (!pending_at_boot) {
        return;
    }

    pending_at_boot = false;
    boot_trace::write("confirmed");
    const auto result = esp_ota_mark_app_valid_cancel_rollback();
    DEBUG_MSG_P(PSTR("[SAFEBOOT] New image confirmed: %s\n"), esp_err_to_name(result));
}

void confirmTick() {
    if (!pending_at_boot) {
        return;
    }

    const auto now = millis();
    if ((now >= ConfirmUptimeMs) && wifiConnected()) {
        confirm();
        return;
    }

    if (now >= ConfirmTimeoutMs) {
        DEBUG_MSG_P(PSTR("[SAFEBOOT] New image not confirmed (no Wi-Fi), back to safeboot\n"));
        delay(100);
        esp_restart();
    }
}

// Crashes of a confirmed image (settings that break a working image). A new image is the bootloader's (rollback).
// Software restarts never count: safeboot hands over to ESPurna with one, and an earlier head that counted them
// went straight back to safeboot (10.0.5.66 11-13)
void guard() {
    pending_at_boot = pending();

    const auto reason = esp_reset_reason();
    if ((rtc_magic != Magic) || (reason == ESP_RST_POWERON) || pending_at_boot) {
        rtc_magic = Magic;
        rtc_count = 0;
    }

    if (pending_at_boot) {
        DEBUG_MSG_P(PSTR("[SAFEBOOT] New image, the bootloader returns to safeboot unless it is confirmed\n"));
        return;
    }

    if (crashed(reason)) {
        ++rtc_count;
    }

    count_at_boot = rtc_count;
    if (rtc_count >= CrashLoopMax) {
        rtc_count = 0;
        DEBUG_MSG_P(PSTR("[SAFEBOOT] %u unstable boots in a row, switching to safeboot\n"), count_at_boot);

        String error;
        if (switchToSafeboot(error)) {
            delay(100);
            esp_restart();
        }

        DEBUG_MSG_P(PSTR("[SAFEBOOT] Switch failed: %s\n"), error.c_str());
    }
}

void tick() {
    confirmTick();
    if (!cleared && (millis() >= CrashLoopUptimeMs)) {
        rtc_count = 0;
        cleared = true;
    }
}

// ESPurna's own safeboot image (code/safeboot-app) reads Wi-Fi, hostname, the admin password and webFwUrl straight
// from these settings. NVS "safeboot" passes the rest: "image" - ESPURNA_IMAGE, its release list shows the files of that
// image; "app" - what app0 holds. No URL: an image goes to safeboot as an upload (the browser), never as a download
namespace request {

String read(const char* key) {
    String out;

    nvs_handle_t handle;
    if (nvs_open("safeboot", NVS_READONLY, &handle) == ESP_OK) {
        size_t size = 0;
        if ((nvs_get_str(handle, key, nullptr, &size) == ESP_OK) && (size > 1)) {
            std::unique_ptr<char[]> value(new (std::nothrow) char[size]);
            if (value && (nvs_get_str(handle, key, value.get(), &size) == ESP_OK)) {
                out = value.get();
            }
        }
        nvs_close(handle);
    }

    return out;
}

// once per image change, not on every boot. "app" - what the safeboot page shows for app0: the app description of
// an Arduino build names the prebuilt IDF libraries ("arduino-lib-builder"), not ESPurna
void image() {
    const String current(F(ESPURNA_IMAGE));

    const auto app = buildApp();
    String description;
    description += app.name.toString();
    description += ' ';
    description += app.version.toString();
    if (current.length()) {
        description += ' ';
        description += current;
    }
    description += F(", built ");
    description += app.build_time.toString();

    if ((current == read("image")) && (description == read("app"))) {
        return;
    }

    nvs_handle_t handle;
    if (nvs_open("safeboot", NVS_READWRITE, &handle) == ESP_OK) {
        if (current.length()) {
            nvs_set_str(handle, "image", current.c_str());
        } else {
            nvs_erase_key(handle, "image");
        }
        nvs_set_str(handle, "app", description.c_str());
        nvs_commit(handle);
        nvs_close(handle);
    }
}

} // namespace request

// SHA-256 of the first `size` bytes of a partition (the image file as it was written), hex
String partitionSha256(const esp_partition_t* part, size_t size) {
    if (!part || !size || (size > part->size)) {
        return String();
    }

    std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[4096]);
    if (!buffer) {
        return String();
    }

    // the one hash library of ESPurna: BearSSL
    br_sha256_context ctx;
    br_sha256_init(&ctx);

    bool ok = true;
    for (size_t offset = 0; ok && (offset < size); offset += 4096) {
        const size_t len = std::min<size_t>(4096, size - offset);
        ok = esp_partition_read(part, offset, buffer.get(), len) == ESP_OK;
        if (ok) {
            br_sha256_update(&ctx, buffer.get(), len);
        }
    }

    uint8_t digest[br_sha256_SIZE];
    br_sha256_out(&ctx, digest);
    if (!ok) {
        return String();
    }

    char out[sizeof(digest) * 2 + 1];
    ::espurna::http::hex(digest, sizeof(digest), out);
    return String(out);
}

// From the two-slot layouts of the Arduino core (min_spiffs.csv, default.csv) to esp32_4mb_safeboot.csv over Wi-Fi.
// ESPurna has to run from the second slot: the safeboot image goes over the first one (0x10000, below the new app0),
// then the partition table at 0x8000 is replaced and otadata erased, so the next boot starts the factory partition.
// The safeboot image (code/safeboot-app) joins Wi-Fi with the ESPurna settings and waits for the upload of ESPurna
// into app0 in its own web UI. NVS (0x9000) and otadata (0xE000) keep their place
namespace layout {

constexpr uint32_t TableOffset { 0x8000 };
constexpr size_t TableSize { 0xC00 };
constexpr uint32_t SafebootOffset { 0x10000 };
constexpr uint32_t SafebootSize { 0xD0000 };
constexpr uint32_t AppOffset { 0xE0000 };
constexpr uint32_t FlashSize { 0x400000 };

struct Entry {
    uint8_t type;
    uint8_t subtype;
    uint32_t offset;
    uint32_t size;
    const char* label;
};

// esp32_4mb_safeboot.csv
constexpr Entry Entries[] {
    {0x01, 0x02, 0x9000, 0x5000, "nvs"},
    {0x01, 0x00, 0xE000, 0x2000, "otadata"},
    {0x00, 0x00, SafebootOffset, SafebootSize, "safeboot"},
    {0x00, 0x10, AppOffset, 0x2D0000, "app0"},
    {0x01, 0x82, 0x3B0000, 0x40000, "spiffs"},
    {0x01, 0x03, 0x3F0000, 0x10000, "coredump"},
};

constexpr uint32_t DataOffset { 0x3B0000 };
constexpr uint32_t DataEnd { 0x3F0000 };

// The data partition stays where it is when it already lies behind app0 (min_spiffs.csv: 128 KB at 0x3D0000).
// Anywhere else (default.csv) it overlaps app0 and becomes the 256 KB one of esp32_4mb_safeboot.csv. All three end at
// 0x3F0000: the settings pool in its top 8 sectors (storage_flash_esp32.cpp) stays where it is
Entry dataEntry(const Entry& entry) {
    const auto* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (part && (part->address >= DataOffset) && ((part->address + part->size) <= DataEnd)) {
        return Entry{entry.type, entry.subtype, part->address, part->size, entry.label};
    }

    return entry;
}

// ESP-IDF partition table: 32-byte entries (0xAA 0x50, type, subtype, offset, size, label[16], flags), then
// the MD5 entry (0xEB 0xEB, 14 x 0xFF, MD5 of the entries above), 0xFF up to the end
void table(uint8_t* out) {
    std::memset(out, 0xFF, TableSize);

    size_t pos { 0 };
    for (const auto& item : Entries) {
        const auto entry = (item.offset == DataOffset) ? dataEntry(item) : item;
        ::espurna::esp32_partitions::write(out + pos, ::espurna::esp32_partitions::Entry{
            entry.type, entry.subtype, entry.offset, entry.size, entry.label});
        pos += 32;
    }

    // MD5 because the table format of ESP-IDF takes it; BearSSL's, the one hash library of ESPurna
    br_md5_context ctx;
    br_md5_init(&ctx);
    br_md5_update(&ctx, out, pos);

    auto* row = out + pos;
    row[0] = 0xEB;
    row[1] = 0xEB;
    std::memset(row + 2, 0xFF, 14);
    br_md5_out(&ctx, row + 16);
}

bool current() {
    const auto* app = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    return factory() && app && (app->address == AppOffset);
}

// the old slot that becomes the safeboot partition
const esp_partition_t* target() {
    const auto* running = esp_ota_get_running_partition();
    const auto* part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    if (part && running && (part != running)
        && (part->address == SafebootOffset) && (part->size >= SafebootSize))
    {
        return part;
    }

    return nullptr;
}

bool possible(String& error) {
    if (current()) {
        error = F("the flash layout is already the safeboot one");
        return false;
    }

    if (factory()) {
        error = F("unknown layout with a factory partition");
        return false;
    }

    uint32_t size { 0 };
    if ((esp_flash_get_size(nullptr, &size) != ESP_OK) || (size < FlashSize)) {
        error = F("needs 4 MB of flash");
        return false;
    }

    const auto* nvs = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, nullptr);
    const auto* otadata = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    if (!nvs || (nvs->address != 0x9000) || (nvs->size != 0x5000)
        || !otadata || (otadata->address != 0xE000) || (otadata->size != 0x2000))
    {
        error = F("NVS / otadata are not at 0x9000 / 0xE000, the settings would be lost");
        return false;
    }

    const auto* running = esp_ota_get_running_partition();
    if (!running || (running->address < AppOffset)) {
        error = F("ESPurna runs from the first slot. Update it once more (any image, it goes to the second slot), then migrate");
        return false;
    }

    if (!target()) {
        error = F("no first OTA slot at 0x10000 big enough for the safeboot image");
        return false;
    }

    return true;
}

// esp_flash_write() refuses (aborts with the Arduino sdkconfig) writes over the partition table. Same chip,
// without the region check, for the one table write
bool writeTable(const uint8_t* data) {
    esp_flash_os_functions_t os = *esp_flash_default_chip->os_func;
    os.region_protected = nullptr;

    esp_flash_t chip = *esp_flash_default_chip;
    chip.os_func = &os;

    std::unique_ptr<uint8_t[]> check(new (std::nothrow) uint8_t[TableSize]);
    if (!check) {
        return false;
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        if ((esp_flash_erase_region(&chip, TableOffset, 0x1000) == ESP_OK)
            && (esp_flash_write(&chip, data, TableOffset, TableSize) == ESP_OK)
            && (esp_flash_read(&chip, check.get(), TableOffset, TableSize) == ESP_OK)
            && (std::memcmp(check.get(), data, TableSize) == 0))
        {
            return true;
        }
    }

    return false;
}

// The safeboot image alone, or the merged image of a release (the whole flash from 0x0, the one file per image):
// of that one only its factory partition is written, found in its own partition table at 0x8000
constexpr size_t MergedTableOffset { 0x8000 };
constexpr size_t MergedTableSize { 0xC00 };

struct Upload {
    const esp_partition_t* part { nullptr };
    size_t written { 0 };
    size_t erased { 0 };
    bool done { false };
    String error;

    size_t received { 0 };
    bool merged { false };
    bool located { false };
    size_t from { 0 };   // factory partition in the merged image
    size_t until { 0 };
    std::unique_ptr<uint8_t[]> table;
};

Upload upload;
String last_error;

bool uploadWrite(const uint8_t* data, size_t len) {
    if ((upload.written + len) > SafebootSize) {
        upload.error = F("the safeboot image is bigger than its partition");
        return false;
    }

    const size_t end = upload.written + len;
    if (end > upload.erased) {
        const size_t erase = ((end - upload.erased) + 0xFFF) & ~size_t(0xFFF);
        if (esp_partition_erase_range(upload.part, upload.erased, erase) != ESP_OK) {
            upload.error = F("erase failed");
            return false;
        }
        upload.erased += erase;
    }

    if (esp_partition_write(upload.part, upload.written, data, len) != ESP_OK) {
        upload.error = F("write failed");
        return false;
    }

    upload.written = end;
    return true;
}

// factory entry of the partition table (entries of 32 bytes: magic 0x50AA, type, subtype, offset, size)
bool uploadLocate() {
    ::espurna::esp32_partitions::Entry factory;
    if (::espurna::esp32_partitions::find(upload.table.get(), MergedTableSize,
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, factory))
    {
        upload.from = factory.offset;
        upload.until = factory.offset + std::min<size_t>(factory.size, SafebootSize);
        upload.located = true;
        upload.table.reset();
        return true;
    }

    upload.error = F("a merged image without a safeboot (factory) partition in its partition table");
    return false;
}

bool uploadFeed(const uint8_t* data, size_t len) {
    if (!len) {
        return true;
    }

    const size_t at = upload.received;
    upload.received += len;
    if (!upload.merged) {
        return uploadWrite(data, len);
    }

    if (!upload.located) {
        const size_t begin = std::max(at, MergedTableOffset);
        const size_t end = std::min(at + len, MergedTableOffset + MergedTableSize);
        if (begin < end) {
            if (!upload.table) {
                upload.table.reset(new (std::nothrow) uint8_t[MergedTableSize]);
                if (!upload.table) {
                    upload.error = F("out of memory");
                    return false;
                }
            }
            std::memcpy(upload.table.get() + (begin - MergedTableOffset), data + (begin - at), end - begin);
        }
        if ((at + len) < (MergedTableOffset + MergedTableSize)) {
            return true;
        }
        if (!uploadLocate()) {
            return false;
        }
    }

    const size_t begin = std::max(at, upload.from);
    const size_t end = std::min(at + len, upload.until);
    if (begin >= end) {
        return true;
    }

    return uploadWrite(data + (begin - at), end - begin);
}

bool uploadVerify() {
    const esp_partition_pos_t pos {
        .offset = upload.part->address,
        .size = upload.part->size,
    };

    esp_image_metadata_t meta;
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK) {
        upload.error = F("the safeboot image does not verify (checksum / hash)");
        return false;
    }

    if ((meta.image.chip_id != ESP_CHIP_ID_ESP32) || (meta.image_len > SafebootSize)) {
        upload.error = F("not an ESP32 image that fits the safeboot partition");
        return false;
    }

    esp_app_desc_t desc;
    if ((esp_ota_get_partition_description(upload.part, &desc) != ESP_OK)
        || (std::strcmp(desc.project_name, "espurna-safeboot") != 0))
    {
        upload.error = F("not the ESPurna safeboot image");
        return false;
    }

    return true;
}

// what a partition holds: "espurna-safeboot 1.0, built ..." or an empty string
String describe(const esp_partition_t* part) {
    esp_app_desc_t desc;
    if (!part || (esp_ota_get_partition_description(part, &desc) != ESP_OK)) {
        return String();
    }

    String out;
    out += desc.project_name;
    out += ' ';
    out += desc.version;
    out += F(", built ");
    out += desc.date;
    out += ' ';
    out += desc.time;
    return out;
}

// in loop(): nothing else touches the flash while the table is replaced
void migrate() {
    std::unique_ptr<uint8_t[]> data(new (std::nothrow) uint8_t[TableSize]);
    if (!data) {
        DEBUG_MSG_P(PSTR("[SAFEBOOT] Migration stopped: out of memory\n"));
        last_error = F("out of memory");
        return;
    }

    table(data.get());
    DEBUG_MSG_P(PSTR("[SAFEBOOT] Writing the partition table, do not power off\n"));
    if (!writeTable(data.get())) {
        // nothing the bootloader can use at 0x8000 now, the next boot needs a serial flash
        DEBUG_MSG_P(PSTR("[SAFEBOOT] PARTITION TABLE WRITE FAILED, do not restart, flash it over serial\n"));
        last_error = F("PARTITION TABLE WRITE FAILED, do not restart, flash it over serial");
        return;
    }

    // blank otadata: the bootloader starts the factory (safeboot) partition. A failed erase is no harm,
    // app0 holds no valid image yet and the bootloader falls back to factory as well
    const auto* otadata = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    if (otadata) {
        esp_partition_erase_range(otadata, 0, otadata->size);
    }

    DEBUG_MSG_P(PSTR("[SAFEBOOT] Layout migrated, restarting into safeboot\n"));
    prepareReset(CustomResetReason::Web);
}

} // namespace layout

#if WEB_SUPPORT
PROGMEM_STRING(NoOtaMessage,
    "ERROR: this flash layout (safeboot) has no second OTA slot. "
    "Updates go through safeboot, see the /safeboot page\n");

void onNoOta(WebRequest& request) {
    webSend(request, 400, "text/plain", String(FPSTR(NoOtaMessage)));
}

void onNoOtaUpload(WebRequest&, const char*, size_t, const uint8_t*, size_t, bool) {
}

PROGMEM_STRING(Page, R"(<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESPurna safeboot</title><style>
body{font-family:sans-serif;max-width:640px;margin:16px auto;padding:0 16px;line-height:1.4}
input[type=text]{width:100%;box-sizing:border-box}button{margin-top:8px;padding:6px 16px}
fieldset{margin-top:16px}.err{color:#b00}.ok{color:#070}#log{white-space:pre-wrap;background:#f4f4f4;padding:8px;margin-top:12px}
</style></head><body><h2>Flash layout and updates</h2><div id="info">...</div>
<div id="old" hidden><fieldset><legend>Migrate to the safeboot layout</legend><p>This board has the old two-slot layout:
the images with Matter (2.1 MB) no longer fit its 1.9 MB slots. The migration puts the ESPurna safeboot image in front
and one 2880 KB slot for ESPurna after it, then restarts into safeboot, which waits for the ESPurna image in its own
web UI (same address, user admin, the admin password). Settings stay.</p>
<p class="err"><b>Keep the power on until ESPurna is back.</b> The flash layout changes: a power cut in between can leave
the board without an image that boots, then only flashing by wire helps. Not on a board in a wall without a way to reach
its UART.</p>
<label>The release image (espurna-esp32-*.bin, its safeboot is used) or a safeboot image<input type="file" id="mfile" accept=".bin"></label>
<button id="migrate">Migrate</button></fieldset></div>
<div id="new" hidden><fieldset><legend>Update ESPurna</legend><p>ESPurna restarts into safeboot, which writes the new
image into the ESPurna slot and starts it.</p>
<p><button id="byfile">Restart into safeboot</button> and upload the image there.</p></fieldset>
<fieldset><legend>Install or replace safeboot</legend>
<p class="err">Keep the power on while safeboot is written: a power cut leaves no safeboot (ESPurna still boots, the
next update needs a new safeboot first).</p><label>The release image (espurna-esp32-*.bin, its safeboot is used) or a safeboot image
<input type="file" id="sfile" accept=".bin"></label><button id="install">Install</button></fieldset></div>
<div id="log" hidden></div>
<script>
const $=id=>document.getElementById(id);let st;
function log(t,c){const l=$('log');l.hidden=false;l.className=c||'';l.textContent=t}
async function status(){const r=await fetch('safeboot/status',{cache:'no-store'});if(!r.ok)throw r.status;return r.json()}
const wait=ms=>new Promise(r=>setTimeout(r,ms));
async function safeboot(t0){for(;;){await wait(3000);const s=Math.round((Date.now()-t0)/1000);
 try{const r=await fetch('/',{cache:'no-store'});if(r.status==401&&(r.headers.get('WWW-Authenticate')||'').indexOf(' safeboot"')>=0){location.href='/';return}}
 catch(e){}log('Restarting into safeboot ('+s+' s)...')}}
function send(path,body){return new Promise((ok,no)=>{const x=new XMLHttpRequest();x.open('POST',path);
 x.upload.onprogress=e=>{if(e.lengthComputable)log('Uploading '+Math.round(e.loaded*100/e.total)+'%')};
 x.onload=()=>(x.status==200?ok:no)(x.responseText);x.onerror=()=>no('connection error');x.send(body)})}
function form(fields){const f=new FormData();for(const k in fields)f.append(k,fields[k]);return f}
$('migrate').onclick=async()=>{const file=$('mfile').files[0];if(!file){log('Pick the safeboot image','err');return}
 if(!confirm('The flash layout changes now. Keep the power on until ESPurna is back: a power cut in between can leave '
  +'the board without an image that boots (then only flashing by wire helps). Continue?'))return;
 const f=form({});f.append('file',file,file.name);
 try{log(await send('safeboot/migrate',f));safeboot(Date.now())}catch(e){log(e,'err')}};
$('byfile').onclick=async()=>{try{log(await send('safeboot/switch',form({})));safeboot(Date.now())}catch(e){log(e,'err')}};
$('install').onclick=async()=>{const file=$('sfile').files[0];if(!file){log('Pick the safeboot image','err');return}
 if(!confirm('Safeboot is written now. Keep the power on until it is done. Continue?'))return;
 const f=new FormData();f.append('file',file,file.name);try{log(await send('safeboot/install',f),'ok');show(await status())}catch(e){log(e,'err')}};
function show(s){st=s;
 $('info').innerHTML='Running: '+s.running+'<br>Layout: '+(s.layout==='old'?'two OTA slots (old)':s.layout)
  +(s.layout==='safeboot'?'<br>Safeboot: '+(s.safeboot||'missing'):'');
 $('old').hidden=s.layout!=='old';$('new').hidden=s.layout!=='safeboot';
 if(s.last)log('Last attempt: '+s.last,'err');if(s.error){log(s.error,'err');$('migrate').disabled=true}}
status().then(show).catch(e=>log(e,'err'));
</script></body></html>)");

bool authenticated(WebRequest& request) {
    if (!webAuthenticate(request)) {
        webRequestAuthentication(request);
        return false;
    }

    return true;
}

// the POSTs that change what the board boots: the login, and not a page of another site (libs/image_receiver.h)
bool sameOrigin(WebRequest& request) {
    return espurna::image::sameOrigin(
        request.header("Origin"), request.header("Host"), request.header("X-Forwarded-Host"));
}

bool allowed(WebRequest& request) {
    if (!authenticated(request)) {
        return false;
    }

    if (!sameOrigin(request)) {
        webSend(request, 403, "text/plain", String(F("ERROR: Origin")));
        return false;
    }

    return true;
}

void onPage(WebRequest& request) {
    if (authenticated(request)) {
        webSend(request, 200, "text/html", String(FPSTR(Page)));
    }
}

void onStatus(WebRequest& request) {
    if (!authenticated(request)) {
        return;
    }

    const auto* running = esp_ota_get_running_partition();
    const bool current = layout::current();

    String error;
    if (!current) {
        layout::possible(error);
    }

    String out;
    out.reserve(512);
    out += F("{\"layout\":\"");
    out += current ? F("safeboot") : (factory() ? F("other") : F("old"));
    out += F("\",\"running\":\"");
    out += running ? running->label : "?";
    // what the factory partition holds: "espurna-safeboot ...", or e.g. Tasmota's safeboot on boards installed over
    // Tasmota (its SHA-256 differs from the release's, the release list installs ours over it first)
    out += F("\",\"safeboot\":\"");
    out += current ? layout::describe(factory()) : String();
    out += F("\",\"error\":\"");
    out += error;
    out += F("\",\"last\":\"");
    out += layout::last_error;
    // ?size=<bytes of the release safeboot>: SHA-256 of the installed one, to compare with the release list
    const char* size = request.param("size");
    if (current && size) {
        out += F("\",\"sha256\":\"");
        out += partitionSha256(factory(), strtoul(size, nullptr, 10));
    }
    out += F("\"}");

    webSend(request, 200, "application/json", std::move(out));
}

void onSwitch(WebRequest& request) {
    if (!allowed(request)) {
        return;
    }

    String error;
    if (!switchToSafeboot(error)) {
        webSend(request, 400, "text/plain", error);
        return;
    }

    webSend(request, 200, "text/plain", String(F("Restarting into safeboot")));
    prepareReset(CustomResetReason::Web);
}

// One handler for both images that go into the safeboot slot: the migration (old layout, into the first OTA slot
// that becomes the factory partition) and install / replace (safeboot layout, into the factory partition itself)
void onSafebootUpload(WebRequest& request, const char*, size_t index, const uint8_t* data, size_t len, bool final) {
    if (!webAuthenticate(request) || !sameOrigin(request)) {
        return;
    }

    if (!index) {
        layout::upload = layout::Upload{};
        String error;
        const bool current = layout::current();
        if (!current && !layout::possible(error)) {
            layout::upload.error = error;
            return;
        }

        // the safeboot image itself, or the merged image of a release starting with the erased area before its
        // bootloader
        if (!len || ((data[0] != 0xE9) && (data[0] != 0xFF))) {
            layout::upload.error = F("not an ESP32 image (no 0xE9 magic byte, no merged image)");
            return;
        }
        layout::upload.merged = data[0] == 0xFF;

        layout::upload.part = current ? factory() : layout::target();
        if (!layout::upload.part) {
            layout::upload.error = F("no partition for the safeboot image");
            return;
        }
    }

    if (!layout::upload.part || layout::upload.error.length() || layout::upload.done) {
        return;
    }

    if (!layout::uploadFeed(const_cast<uint8_t*>(data), len)) {
        layout::upload.part = nullptr;
        return;
    }

    if (final && layout::uploadVerify()) {
        layout::upload.done = true;
    }
}

bool uploadDone(WebRequest& request) {
    if (!layout::upload.done) {
        webSend(request, 400, "text/plain",
            layout::upload.error.length() ? layout::upload.error : String(F("no safeboot image received")));
        return false;
    }

    layout::upload.done = false;
    return true;
}

void onInstall(WebRequest& request) {
    if (!allowed(request)) {
        return;
    }

    if (!layout::current()) {
        webSend(request, 400, "text/plain", String(F("not the safeboot layout, migrate first")));
        return;
    }

    if (uploadDone(request)) {
        webSend(request, 200, "text/plain", String(F("Safeboot installed: ")) + layout::describe(factory()));
    }
}

void onMigrate(WebRequest& request) {
    if (!allowed(request)) {
        return;
    }

    if (!uploadDone(request)) {
        return;
    }

    espurnaRegisterOnce(layout::migrate);

    webSend(request, 200, "text/plain",
        String(F("Safeboot image written. Writing the partition table, then restarting into safeboot")));
}
#endif

#if TERMINAL_SUPPORT
PROGMEM_STRING(Safeboot, "SAFEBOOT");

void command_safeboot(::terminal::CommandContext&& ctx) {
    String error;
    if (!switchToSafeboot(error)) {
        terminalError(ctx, error);
        return;
    }

    ctx.output.print(F("Next boot: factory (safeboot) partition, restarting\n"));
    terminalOK(ctx);
    prepareReset(CustomResetReason::Terminal);
}

PROGMEM_STRING(Layout, "SAFEBOOT.STATUS");

void command_status(::terminal::CommandContext&& ctx) {
    const auto* running = esp_ota_get_running_partition();
    ctx.output.printf_P(PSTR("running: %s, factory (safeboot): %s, second OTA slot: %s, unstable boots: %u\n"),
        running ? running->label : "?",
        factory() ? "present" : "missing",
        otaPossible() ? "available" : "missing",
        count_at_boot);

    if (factory()) {
        const auto image = layout::describe(factory());
        ctx.output.printf_P(PSTR("factory image: %s\n"), image.length() ? image.c_str() : "none (erased or not an app)");
    }

    if (!layout::current()) {
        String error;
        ctx.output.printf_P(PSTR("old layout, migration over Wi-Fi (web /safeboot): %s\n"),
            layout::possible(error) ? "possible" : error.c_str());
    }

    terminalOK(ctx);
}

// NVS is 20 KB in this layout (and in min_spiffs.csv): Wi-Fi, Matter (chip-*, espurna-id), the safeboot settings; the
// ESPurna settings blob of before until it is taken over (storage_flash_esp32.cpp). What takes it, per namespace
PROGMEM_STRING(Nvs, "NVS");

void command_nvs(::terminal::CommandContext&& ctx) {
    nvs_stats_t stats;
    if (nvs_get_stats(nullptr, &stats) == ESP_OK) {
        ctx.output.printf_P(PSTR("entries: used %u, free %u, available %u, total %u, namespaces %u\n"),
            stats.used_entries, stats.free_entries, stats.available_entries, stats.total_entries,
            stats.namespace_count);
    }

    struct Space {
        String name;
        size_t keys;
        size_t entries;
    };

    std::vector<Space> spaces;
    nvs_iterator_t it { nullptr };
    auto result = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);
    while (result == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        size_t used { 0 };
        nvs_handle_t handle;
        if (nvs_open(info.namespace_name, NVS_READONLY, &handle) == ESP_OK) {
            nvs_get_used_entry_count(handle, &used);
            nvs_close(handle);
        }

        auto found = std::find_if(spaces.begin(), spaces.end(),
            [&](const Space& space) { return space.name == info.namespace_name; });
        if (found == spaces.end()) {
            spaces.push_back(Space{info.namespace_name, 1, used});
        } else {
            ++found->keys;
        }

        result = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);

    for (const auto& space : spaces) {
        ctx.output.printf_P(PSTR("%-16s keys %3u, entries %4u (~%u bytes)\n"),
            space.name.c_str(), space.keys, space.entries, space.entries * 32);
    }

    terminalOK(ctx);
}

PROGMEM_STRING(NvsErase, "NVS.ERASE");

void command_nvs_erase(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() != 2) {
        terminalError(ctx, F("NVS.ERASE <namespace>"));
        return;
    }

    nvs_handle_t handle;
    if (nvs_open(ctx.argv[1].c_str(), NVS_READWRITE, &handle) != ESP_OK) {
        terminalError(ctx, F("no such namespace"));
        return;
    }

    const auto result = nvs_erase_all(handle);
    const auto commit = nvs_commit(handle);
    nvs_close(handle);

    if ((result != ESP_OK) || (commit != ESP_OK)) {
        terminalError(ctx, String(esp_err_to_name((result != ESP_OK) ? result : commit)));
        return;
    }

    terminalOK(ctx);
}

static constexpr ::terminal::Command Commands[] PROGMEM {
    {Safeboot, command_safeboot},
    {Layout, command_status},
    {Nvs, command_nvs},
    {NvsErase, command_nvs_erase},
};
#endif

} // namespace
} // namespace safeboot
} // namespace espurna

void safebootTrace(const char* name) {
    espurna::boot_trace::write(name);
}

// before every static initializer of ESPurna: only the RTC memory is written
__attribute__((constructor(101))) static void safebootTraceCtors() {
    espurna::boot_trace::write("ctors");
}

// right after rtcmem
void safebootGuard() {
    espurna::safeboot::guard();
}

// ESPurna restarts by itself: its loop runs, a new image is good enough to stay
void safebootConfirm() {
    espurna::safeboot::confirm();
}

// Arduino confirms a new image before setup() (initArduino) unless this says it is ours to do
extern "C" bool verifyRollbackLater() {
    return true;
}

void safebootSetup() {
    using namespace espurna::safeboot;

    request::image();
    espurnaRegisterLoop(tick);

#if TERMINAL_SUPPORT
    espurna::terminal::add(Commands);
#endif
}

#if WEB_SUPPORT
// Only after webSetup(): the server does not exist before it (and never in the unstable mode)
void safebootWebSetup() {
    using namespace espurna::safeboot;

    auto& server = webServer();
    // a handler takes its sub-paths as well ("/safeboot" would answer "/safeboot/status"): specific ones first
    server.on("/safeboot/status", WebMethod::Get, onStatus);
    server.on("/safeboot/switch", WebMethod::Post, onSwitch);
    server.on("/safeboot/migrate", WebMethod::Post, onMigrate, onSafebootUpload);
    server.on("/safeboot/install", WebMethod::Post, onInstall, onSafebootUpload);
    server.on("/safeboot", WebMethod::Get, onPage);

    if (!otaPossible()) {
        server.rewrite("/upgrade", "/safeboot-noota");
        server.on("/safeboot-noota", WebMethod::Any, onNoOta, onNoOtaUpload);
        DEBUG_MSG_P(PSTR("[SAFEBOOT] No second OTA slot, /upgrade is disabled (use /safeboot)\n"));
    }
}
#endif

#endif // SAFEBOOT_SUPPORT
