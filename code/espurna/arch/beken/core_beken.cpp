/*

The SDK calls of the chip that ESPurna makes directly (arch/rtos/chip_sdk.h, wifi_sdk.h: Wi-Fi, system, time, GPIO,
ADC, PWM, UART, I2C, SPI, OTA) and the rest of arch/beken's (beken_api.h: flash, heap, watchdog, reboot), Beken on the
BDK alone (owner 2026-11-18 «LibreTiny уже выкините»). The BDK calls follow what LibreTiny 1.14 did over it (its
cores/beken-72xx, MIT, Kuba Szczodrzyński): the Wi-Fi of WiFiGeneric/STA/AP/Scan/Events.cpp, the UART of Serial.cpp,
the pins of wiring_*.c, the flash of fal_flash_bk72xx_port.c. I2C and SPI are software masters on any two/three pins
(were lib_beken/Wire and SPI)

*/

#include "bdk/bdk.h"

#include "../rtos/chip_sdk.h"
#include "../rtos/wifi_sdk.h"
#include "beken_api.h"

#include <lwip/inet.h>
#include <sys/time.h>
#include <lwip/netif.h>

#include <FreeRTOS.h>
#include <queue.h>
#include <task.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// as linked (sketch_size()), ref. bdk/bk7231n_bsp.ld
extern "C" char _data_flash_begin;
extern "C" char _data_ram_begin;
extern "C" char _data_ram_end;

// the C++ constructors, before anything of ESPurna runs
extern "C" void __libc_init_array(void);

// lwIP port of the BDK (net.c): the hook it calls when the station has its address, static or from DHCP
extern "C" void net_set_sta_ipup_callback(void* fn);

// BDK supplicant control (wlan_ui_pub.h): stops the station, the supplicant included
extern "C" int wlan_sta_disconnect(void);

// the end of the linker's RAM sections (bdk/bk7231n_bsp.ld), a C name: declared inside the namespace it was
// espurna::beken::_empty_ram, an undefined reference
extern "C" unsigned char _empty_ram;

namespace espurna {
namespace beken {
namespace {

// --- time ----------------------------------------------------------------------------------------------------------

constexpr uint32_t TicksPerUs { CFG_XTAL_FREQUENCE / 1000 / 1000 };
constexpr uint32_t UsPerOverflow { portTICK_PERIOD_MS * 1000 };
constexpr uint32_t TicksPerOverflow { TicksPerUs * UsPerOverflow };

// the microsecond timer of the scheduler tick (the SDK glue fixups/clock_rtos.c: BK_TIMER_ID0 counts us to one tick)
uint32_t timer_ticks() {
    uint32_t timeout = 0;
    REG_WRITE(TIMER0_2_READ_CTL, (BKTIMER0 << 2) | 1);
    while (REG_READ(TIMER0_2_READ_CTL) & 1) {
        if (++timeout > (120 * 1000)) {
            return 0;
        }
    }

    return REG_READ(TIMER0_2_READ_VALUE);
}

uint32_t millis() {
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

uint32_t micros32() {
    static uint32_t last_millis { 0 };
    static uint32_t corrected { 0 };
    static uint32_t last_ticks { 0 };

    const uint32_t now_millis = millis();
    const uint32_t now_ticks = timer_ticks();
    if (now_millis != last_millis) {
        corrected = now_millis;
    } else if (now_ticks < last_ticks) {
        // the tick counter rolled over while the scheduler tick was held off (interrupts off)
        corrected += portTICK_PERIOD_MS;
    }

    last_millis = now_millis;
    last_ticks = now_ticks;
    return corrected * 1000 + now_ticks / TicksPerUs;
}

// 32 bit micros wrap every ~71 minutes, called often enough (every loop) that one wrap between two calls is the only one
uint64_t micros64() {
    static uint32_t last { 0 };
    static uint32_t high { 0 };

    const auto level = port_disable_interrupts_flag();
    const uint32_t now = micros32();
    if (now < last) {
        ++high;
    }
    last = now;
    const uint64_t out = (uint64_t{high} << 32) | now;
    port_enable_interrupts_flag(level);

    return out;
}

void delay_us(uint32_t us) {
    if (!us) {
        return;
    }

    --us;
    const uint32_t start = timer_ticks();
    const uint32_t start2 = start - TicksPerOverflow;
    uint32_t ticks = TicksPerUs * us;
    while (ticks > TicksPerOverflow) {
        while (timer_ticks() > start) {
        }
        while (timer_ticks() < start) {
        }
        ticks -= TicksPerOverflow;
    }

    while (((timer_ticks() - start) < ticks) || ((timer_ticks() - start2) < ticks)) {
    }
}

// --- flash ---------------------------------------------------------------------------------------------------------

constexpr uint32_t FlashSector { 4096 };

// can't include <flash.h> of the BDK (it collides with <Flash.h> on Windows)
constexpr uint32_t RegFlashBase { 0x00803000 };
constexpr uint32_t RegFlashOperateSw { RegFlashBase + 0 * 4 };
constexpr uint32_t RegFlashRdid { RegFlashBase + 4 * 4 };
constexpr uint32_t FlashBusySw { 0x01UL << 31 };
constexpr uint32_t FlashWpValue { 0x01UL << 30 };
constexpr uint32_t FlashOpSw { 0x01UL << 29 };
constexpr uint32_t FlashOpTypePos { 24 };
constexpr uint32_t FlashOpRdid { 20 };

void flash_unprotect() {
    if (get_flash_protect() != FLASH_PROTECT_NONE) {
        flash_protection_op(0, FLASH_PROTECT_NONE);
    }
}

bool flash_inside(uint32_t offset, size_t length) {
    const uint32_t size = espurna::beken::flash_size();
    return (offset <= size) && (length <= (size - offset));
}

} // namespace

uint32_t flash_id() {
    static uint32_t id { 0 };
    if (!id) {
        const uint32_t data = (FlashOpRdid << FlashOpTypePos) | FlashOpSw | FlashWpValue;
        REG_WRITE(RegFlashOperateSw, data);
        while (REG_READ(RegFlashOperateSw) & FlashBusySw) {
        }

        // manufacturer, chip, size: the order of the RDID answer
        const uint32_t value = REG_READ(RegFlashRdid);
        id = ((value >> 16) & 0xff) << 16 | ((value >> 8) & 0xff) << 8 | (value & 0xff);
    }

    return id;
}

// the size code of the RDID answer, 2^n bytes (1 MiB .. 32 MiB); the Tuya layout's 2 MiB when it is not one of those
uint32_t flash_size() {
    static uint32_t size { 0 };
    if (!size) {
        const uint8_t code = flash_id() & 0xff;
        size = ((code >= 0x14) && (code <= 0x19)) ? (1UL << code) : FLASH_LENGTH;
    }

    return size;
}

uint32_t flash_read(uint32_t offset, uint8_t* data, size_t length) {
    if (!flash_inside(offset, length)) {
        return 0;
    }

    ::flash_read(reinterpret_cast<char*>(data), length, offset);
    return length;
}

uint32_t flash_write(uint32_t offset, const uint8_t* data, size_t length) {
    if (!flash_inside(offset, length)) {
        return 0;
    }

    flash_unprotect();
    ::flash_write(reinterpret_cast<char*>(const_cast<uint8_t*>(data)), length, offset);
    return length;
}

bool flash_erase(uint32_t offset, size_t length) {
    if (!length || !flash_inside(offset, length)) {
        return false;
    }

    flash_unprotect();
    const uint32_t first = offset & ~(FlashSector - 1);
    const uint32_t end = offset + length;
    for (uint32_t address = first; address < end; address += FlashSector) {
        uint32_t sector = address;
        flash_ctrl(CMD_FLASH_ERASE_SECTOR, &sector);
    }

    return true;
}

bool flash_erase_block(uint32_t offset) {
    return flash_erase(offset, 1);
}

// --- heap, system ----------------------------------------------------------------------------------------------------

uint32_t heap_free() {
    return xPortGetFreeHeapSize();
}

uint32_t heap_min_free() {
    return xPortGetMinimumEverFreeHeapSize();
}

// the heap of the BDK's FreeRTOS port (heap_4) does not tell its largest block
uint32_t heap_max_alloc() {
    return 0;
}

// the heap is what the linker left of the 192 KiB of RAM (configDYNAMIC_HEAP_SIZE of the port)
uint32_t heap_size() {
    return (0x00400000 + 192 * 1024) - reinterpret_cast<uint32_t>(&_empty_ram);
}

Reboot reboot_reason() {
    switch (bk_misc_get_start_type()) {
    case RESET_SOURCE_POWERON:
        return Reboot::Power;
    case RESET_SOURCE_REBOOT:
        return Reboot::Software;
    case RESET_SOURCE_WATCHDOG:
        return Reboot::Watchdog;
    case RESET_SOURCE_CRASH_XAT0:
    case RESET_SOURCE_CRASH_UNDEFINED:
    case RESET_SOURCE_CRASH_PREFETCH_ABORT:
    case RESET_SOURCE_CRASH_DATA_ABORT:
    case RESET_SOURCE_CRASH_UNUSED:
    case RESET_SOURCE_CRASH_PER_XAT0:
        return Reboot::Crash;
    case RESET_SOURCE_DEEPPS_USB:
        return Reboot::SleepUsb;
    case RESET_SOURCE_DEEPPS_GPIO:
        return Reboot::SleepGpio;
    case RESET_SOURCE_DEEPPS_RTC:
        return Reboot::SleepRtc;
    default:
        break;
    }

    return Reboot::Unknown;
}

// the names LibreTiny gave (the reset reason text of the boards in the field stays the same)
const char* reboot_reason_name(Reboot reason) {
    switch (reason) {
    case Reboot::Power:
        return "Power-On";
    case Reboot::Software:
        return "SW Reboot";
    case Reboot::Watchdog:
        return "WDT Reset";
    case Reboot::Crash:
        return "Crash";
    case Reboot::SleepGpio:
        return "Sleep Wakeup (GPIO)";
    case Reboot::SleepRtc:
        return "Sleep Wakeup (RTC)";
    case Reboot::SleepUsb:
        return "Sleep Wakeup (USB)";
    case Reboot::Unknown:
        break;
    }

    return "Unknown";
}

void reboot() {
    bk_reboot();
    for (;;) {
    }
}

namespace raw {

void wdt_enable(uint32_t timeout) {
    wdt_ctrl(WCMD_SET_PERIOD, &timeout);
    wdt_ctrl(WCMD_POWER_UP, nullptr);
}

void wdt_disable() {
    wdt_ctrl(WCMD_POWER_DOWN, nullptr);
}

void wdt_feed() {
    wdt_ctrl(WCMD_RELOAD_PERIOD, nullptr);
}

} // namespace raw

void device_mac(uint8_t* mac) {
    cfg_load_mac(mac);
}

void rand_bytes(uint8_t* out, size_t length) {
    while (length) {
        const int value = rand();
        const size_t size = std::min(length, sizeof(value));
        std::memcpy(out, &value, size);
        out += size;
        length -= size;
    }
}

// The true random number generator of the BK7231N (its registers, as the TRNG driver of the Armino SDK): 4 samples 20 us
// apart folded into each word. The entropy of mbedTLS (mbedtls_hardware_poll below: the head's TLS, the DRBG of Matter)
void trng_bytes(uint8_t* out, size_t length) {
    constexpr uintptr_t Ctrl { 0x00802480 };
    constexpr uintptr_t Data { 0x00802484 };
    constexpr uint32_t Enable { 1 };

    auto* ctrl = reinterpret_cast<volatile uint32_t*>(Ctrl);
    const auto* data = reinterpret_cast<const volatile uint32_t*>(Data);

    *ctrl = *ctrl | Enable;
    while (length) {
        uint32_t word { 0 };
        for (size_t index = 0; index < 4; ++index) {
            delay_us(20);
            word = (word << 7) ^ (word >> 25) ^ *data;
        }

        const auto size = std::min(length, sizeof(word));
        std::memcpy(out, &word, size);
        out += size;
        length -= size;
    }
    *ctrl = *ctrl & ~Enable;
}

const char* cpu_model() {
    return "BK7231N";
}

uint32_t cpu_mhz() {
    return configCPU_CLOCK_HZ / 1000000;
}

// FreeRTOS ticks scaled to the CPU clock, 2 ms resolution
uint32_t cpu_cycle_count() {
    return xTaskGetTickCount() * (configCPU_CLOCK_HZ / configTICK_RATE_HZ);
}

const char* bdk_version() {
    return "BDK 3.0.33";
}

} // namespace beken
} // namespace espurna

// The wall clock of newlib (no LibreTiny any more, the BDK has none): the 64-bit microseconds of the scheduler's timer
// plus an offset that settimeofday() sets (lwIP SNTP, the SDK glue lwipopts/lwipopts.h; the ntp module). As ESP-IDF does
// it, newlib itself has no settimeofday() and calls the platform's _gettimeofday()
namespace {

int64_t wall_offset_us { 0 };

} // namespace

extern "C" int _gettimeofday(struct timeval* tv, void*) {
    if (tv) {
        const int64_t now = static_cast<int64_t>(espurna::beken::micros64()) + wall_offset_us;
        tv->tv_sec = static_cast<time_t>(now / 1000000);
        tv->tv_usec = static_cast<suseconds_t>(now % 1000000);
    }

    return 0;
}

extern "C" int settimeofday(const struct timeval* tv, const struct timezone*) {
    if (tv) {
        const int64_t wall = (static_cast<int64_t>(tv->tv_sec) * 1000000) + tv->tv_usec;
        wall_offset_us = wall - static_cast<int64_t>(espurna::beken::micros64());
    }

    return 0;
}

// MBEDTLS_ENTROPY_HARDWARE_ALT of bdk/config/tls_config.h: the one entropy source of mbedTLS on the chip
extern "C" int mbedtls_hardware_poll(void*, unsigned char* output, size_t len, size_t* olen) {
    espurna::beken::trng_bytes(output, len);
    *olen = len;
    return 0;
}

namespace espurna {

namespace core {
namespace chip {
namespace {

// --- Wi-Fi ---------------------------------------------------------------------------------------------------------

struct Wifi {
    network_InitTypeDef_adv_st sta;
    network_InitTypeDef_ap_st ap;
    LinkStatusTypeDef link;
    IPStatusTypedef ip;
    char hostname[33];
    bool ap_enabled;
    bool sleep;
};

Wifi& wifi() {
    static Wifi out = [] {
        Wifi tmp{};
        tmp.sta.dhcp_mode = DHCP_CLIENT;
        return tmp;
    }();
    return out;
}

WifiEventHandler handler { nullptr };
QueueHandle_t events { nullptr };
TaskHandle_t event_task { nullptr };

// the scan of the BDK, from its callback
struct Scan {
    std::vector<WifiInfo> records;
    std::atomic<bool> running { false };
    uint32_t timeout { 0 };
};

Scan& scan() {
    static Scan out;
    return out;
}

constexpr int ScanDoneEvent { 0x80 };

wifi_auth_mode_t auth_mode(uint8_t security) {
    switch (security) {
    case BK_SECURITY_TYPE_NONE:
        return WIFI_AUTH_OPEN;
    case BK_SECURITY_TYPE_WEP:
        return WIFI_AUTH_WEP;
    case BK_SECURITY_TYPE_WPA_TKIP:
    case BK_SECURITY_TYPE_WPA_AES:
        return WIFI_AUTH_WPA_PSK;
    case BK_SECURITY_TYPE_WPA2_TKIP:
    case BK_SECURITY_TYPE_WPA2_AES:
    case BK_SECURITY_TYPE_WPA2_MIXED:
        return WIFI_AUTH_WPA2_PSK;
#ifdef BK_SECURITY_TYPE_WPA3_SAE
    case BK_SECURITY_TYPE_WPA3_SAE:
        return WIFI_AUTH_WPA3_PSK;
    case BK_SECURITY_TYPE_WPA3_WPA2_MIXED:
        return WIFI_AUTH_WPA2_WPA3_PSK;
#endif
    default:
        break;
    }

    return WIFI_AUTH_INVALID;
}

bool link_status() {
    if (!sta_ip_is_start()) {
        return false;
    }

    auto& state = wifi();
    std::memset(&state.link, 0, sizeof(state.link));
    bk_wlan_get_link_status(&state.link);
    return true;
}

bool ip_status(uint8_t iface) {
    if (!(iface == BK_STATION ? sta_ip_is_start() : uap_ip_is_start())) {
        return false;
    }

    auto& state = wifi();
    std::memset(&state.ip, 0, sizeof(state.ip));
    bk_wlan_get_ip_status(&state.ip, static_cast<WiFi_Interface>(iface));
    return true;
}

uint32_t address(const char* text) {
    return text[0] ? ipaddr_addr(text) : 0;
}

void address_text(char* out, size_t size, uint32_t value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    std::snprintf(out, size, "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
}

void copy_ssid(uint8_t (&out)[33], const char* ssid) {
    std::memset(out, 0, sizeof(out));
    std::memcpy(out, ssid, strnlen(ssid, 32));
}

void event(int value) {
    if (!handler) {
        return;
    }

    WifiEventInfo out{};
    switch (value) {
    case RW_EVT_STA_CONNECTED:
        if (link_status()) {
            std::memcpy(out.bssid, wifi().link.bssid, sizeof(out.bssid));
        }
        handler(WifiEvent::StaConnected, out);
        break;

    case RW_EVT_STA_BEACON_LOSE:
        out.reason = WIFI_REASON_BEACON_TIMEOUT;
        handler(WifiEvent::StaDisconnected, out);
        break;

    case RW_EVT_STA_PASSWORD_WRONG:
        out.reason = WIFI_REASON_AUTH_FAIL;
        handler(WifiEvent::StaDisconnected, out);
        break;

    case RW_EVT_STA_NO_AP_FOUND:
        out.reason = WIFI_REASON_NO_AP_FOUND;
        handler(WifiEvent::StaDisconnected, out);
        break;

    case RW_EVT_STA_ASSOC_FULL:
        out.reason = WIFI_REASON_ASSOC_TOOMANY;
        handler(WifiEvent::StaDisconnected, out);
        break;

    case RW_EVT_STA_DISCONNECTED:
        out.reason = WIFI_REASON_ASSOC_LEAVE;
        handler(WifiEvent::StaDisconnected, out);
        break;

    case RW_EVT_STA_CONNECT_FAILED:
        out.reason = WIFI_REASON_CONNECTION_FAIL;
        handler(WifiEvent::StaDisconnected, out);
        break;

    case RW_EVT_STA_GOT_IP:
        handler(WifiEvent::StaGotIp, out);
        break;

    case ScanDoneEvent:
        handler(WifiEvent::ScanDone, out);
        break;

    default:
        break;
    }
}

// the BDK's callback runs on its own task, the handler on this one (as LibreTiny's "wifievent")
void event_send(int value) {
    if (events) {
        xQueueSend(events, &value, portMAX_DELAY);
    }
}

void status_callback(rw_evt_type* value) {
    event_send(static_cast<int>(*value));
}

// A static address: the lwIP port of the BDK (net.c wm_netif_status_static_callback) only sets its own station status
// to 'got IP', the status callback above never hears of it (its DHCP branch calls it), and the driver dropped the link
// at the end of every attempt, 15 s (214, 2026-11-23). Its 'IP up' hook runs in both branches: the DHCP one is the
// status callback's already
void static_ipup(void*) {
    if (wifi().sta.dhcp_mode == DHCP_DISABLE) {
        event_send(RW_EVT_STA_GOT_IP);
    }
}

void event_loop(void*) {
    int value;
    for (;;) {
        if (xQueueReceive(events, &value, portMAX_DELAY) == pdTRUE) {
            event(value);
        }
    }
}

void events_start() {
    if (!events) {
        events = xQueueCreate(32, sizeof(int));
    }

    if (!event_task) {
        xTaskCreate(event_loop, "wifievent", 512, nullptr, 4, &event_task);
    }

    bk_wlan_status_register_cb(reinterpret_cast<FUNC_1PARAM_PTR>(status_callback));
    net_set_sta_ipup_callback(reinterpret_cast<void*>(static_ipup));
}

// the RF and the BDK's apps, once (LibreTiny: the first WiFi.mode())
void rf_init() {
    if (__bk_rf_is_init) {
        return;
    }

    func_init_extended();
    app_pre_start();
    while (xTaskGetHandle("init_thread")) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    __bk_rf_is_init = 1;
}

void scan_handler(void*, uint8_t) {
    auto& state = scan();

    ScanResult_adv result;
    result.ApNum = 0;
    result.ApList = nullptr;
    if (!wlan_sta_scan_result(&result)) {
        state.records.clear();
        state.records.reserve(result.ApNum);
        for (int index = 0; index < result.ApNum; ++index) {
            const auto& ap = result.ApList[index];
            WifiInfo info{};
            copy_ssid(info.ssid, ap.ssid);
            std::memcpy(info.bssid, ap.bssid, sizeof(info.bssid));
            info.channel = ap.channel;
            info.rssi = ap.ApPower;
            info.authmode = auth_mode(ap.security);
            state.records.push_back(info);
        }
    }

    if (wifi().sleep) {
        bk_wlan_dtim_rf_ps_mode_enable();
    }

    if (result.ApList) {
        std::free(result.ApList);
    }

    state.running = false;
    event_send(ScanDoneEvent);
}

} // namespace

void wifi_on_event(WifiEventHandler value) {
    handler = value;
    events_start();
}

wifi_mode_t wifi_get_mode() {
    const int sta = bk_wlan_has_role(VIF_STA) ? WIFI_MODE_STA : 0;
    const int ap = wifi().ap_enabled ? WIFI_MODE_AP : 0;
    return static_cast<wifi_mode_t>(sta | ap);
}

bool wifi_mode(wifi_mode_t mode) {
    auto& state = wifi();
    const auto current = wifi_get_mode();
    const bool sta = (mode & WIFI_MODE_STA) != 0;
    const bool ap = (mode & WIFI_MODE_AP) != 0;

    events_start();
    rf_init();

    if (mode) {
        uint32_t reg = 1;
        sddev_control(SCTRL_DEV_NAME, CMD_RF_HOLD_BIT_SET, &reg);
    }

    if (sta && !(current & WIFI_MODE_STA)) {
        bk_wlan_sta_init(nullptr);
        if (state.sleep) {
            // bk_wlan_stop(BK_STATION) turns the DTIM sleep off, nothing turns it back on
            bk_wlan_dtim_rf_ps_mode_enable();
        }
    } else if (!sta && (current & WIFI_MODE_STA)) {
        bk_wlan_stop(BK_STATION);
    }

    // the AP is only started by wifi_ap_start(): on the BK7231, enabling it without starting breaks the station
    if (ap) {
        state.ap_enabled = true;
    } else if (state.ap_enabled) {
        bk_wlan_stop(BK_SOFT_AP);
        state.ap_enabled = false;
    }

    return true;
}

bool wifi_sleep(bool enabled) {
    // DTIM sleep only: the MCU sleep stops the loop task
    if (enabled) {
        bk_wlan_dtim_rf_ps_mode_enable();
    } else {
        bk_wlan_dtim_rf_ps_mode_disable();
        bk_wlan_mcu_ps_mode_disable();
    }

    wifi().sleep = enabled;
    return true;
}

bool wifi_sta_begin(const char* ssid, const char* passphrase, uint8_t channel, const uint8_t* bssid) {
    if (!ssid || !ssid[0] || (std::strlen(ssid) > 32)) {
        return false;
    }

    if (passphrase && (std::strlen(passphrase) > 64)) {
        return false;
    }

    if (!(wifi_get_mode() & WIFI_MODE_STA)) {
        wifi_mode(static_cast<wifi_mode_t>(wifi_get_mode() | WIFI_MODE_STA));
    }

    bk_wlan_connection_loss();

    auto& sta = wifi().sta;
    std::memset(sta.ap_info.ssid, 0, sizeof(sta.ap_info.ssid));
    std::strncpy(sta.ap_info.ssid, ssid, sizeof(sta.ap_info.ssid) - 1);
    if (passphrase) {
        sta.key_len = std::strlen(passphrase);
        std::memcpy(sta.key, passphrase, sta.key_len);
    } else {
        sta.key[0] = '\0';
        sta.key_len = 0;
    }

    sta.ap_info.channel = channel;
    sta.wifi_retry_interval = 100;
    if (bssid) {
        std::memcpy(sta.ap_info.bssid, bssid, 6);
    } else {
        std::memset(sta.ap_info.bssid, 0, 6);
    }

    beken::wdt_feed();
    bk_wlan_start_sta_adv_fix(&sta);
    beken::wdt_feed();
    return true;
}

bool wifi_sta_disconnect() {
    bk_wlan_connection_loss();
    return true;
}

bool wifi_sta_config(uint32_t ip, uint32_t gateway, uint32_t netmask, uint32_t dns1, uint32_t) {
    auto& sta = wifi().sta;
    sta.dhcp_mode = ip ? DHCP_DISABLE : DHCP_CLIENT;
    if (ip) {
        address_text(sta.local_ip_addr, sizeof(sta.local_ip_addr), ip);
        address_text(sta.net_mask, sizeof(sta.net_mask), netmask);
        address_text(sta.gateway_ip_addr, sizeof(sta.gateway_ip_addr), gateway);
        if (dns1) {
            address_text(sta.dns_server_ip_addr, sizeof(sta.dns_server_ip_addr), dns1);
        } else {
            sta.dns_server_ip_addr[0] = '\0';
        }
    } else {
        sta.local_ip_addr[0] = '\0';
        sta.net_mask[0] = '\0';
        sta.gateway_ip_addr[0] = '\0';
        sta.dns_server_ip_addr[0] = '\0';
    }

    if (sta_ip_is_start()) {
        sta_ip_down();
        ip_address_set(BK_STATION, sta.dhcp_mode,
            sta.local_ip_addr, sta.net_mask, sta.gateway_ip_addr, sta.dns_server_ip_addr);
        sta_ip_start();
    }

    return true;
}

bool wifi_sta_address(uint32_t& ip, uint32_t& netmask, uint32_t& gateway) {
    if (!ip_status(BK_STATION)) {
        ip = netmask = gateway = 0;
        return false;
    }

    const auto& state = wifi().ip;
    ip = address(state.ip);
    netmask = address(state.mask);
    gateway = address(state.gate);
    return true;
}

bool wifi_sta_info(WifiInfo& out) {
    if (!link_status()) {
        return false;
    }

    const auto& link = wifi().link;
    copy_ssid(out.ssid, reinterpret_cast<const char*>(link.ssid));
    std::memcpy(out.bssid, link.bssid, sizeof(out.bssid));
    out.channel = static_cast<uint8_t>(link.channel);
    out.rssi = static_cast<int8_t>(link.wifi_strength);
    out.authmode = auth_mode(link.security);
    return true;
}

void wifi_hostname_apply();

} // namespace chip
} // namespace core
} // namespace espurna

// The BDK adds the station netif anew on every association (lwIP port net.c net_wlan_add_netif, called by the
// supplicant): its low_level_init() names it "wlan0" and the DHCP request carried that instead of the hostname set
// before the connect (214, 2026-11-23). Linked with --wrap=net_wlan_add_netif (platformio_beken.ini)
extern "C" {

void __real_net_wlan_add_netif(void* mac);

void __wrap_net_wlan_add_netif(void* mac) {
    __real_net_wlan_add_netif(mac);
    ::espurna::core::chip::wifi_hostname_apply();
}

} // extern "C"

namespace espurna {
namespace core {
namespace chip {

void wifi_hostname_apply() {
    auto* ifs = static_cast<struct netif*>(net_get_sta_handle());
    if (ifs && wifi().hostname[0]) {
        netif_set_hostname(ifs, wifi().hostname);
    }
}

bool wifi_hostname(const char* hostname) {
    auto& state = wifi();
    std::strncpy(state.hostname, hostname, sizeof(state.hostname) - 1);
    auto* ifs = static_cast<struct netif*>(net_get_sta_handle());
    if (ifs) {
        netif_set_hostname(ifs, state.hostname);
    }

    return true;
}

const char* wifi_get_hostname() {
    auto* ifs = static_cast<struct netif*>(net_get_sta_handle());
    return ifs ? netif_get_hostname(ifs) : wifi().hostname;
}

bool wifi_mac(bool ap, uint8_t* out) {
    wifi_get_mac_address(reinterpret_cast<char*>(out), ap ? CONFIG_ROLE_AP : CONFIG_ROLE_STA);
    return true;
}

// the station's: the SDK reads the MAC of the RF calibration block once, a new one is set for this boot
// (system_beken.cpp)
bool wifi_set_mac(bool ap, const uint8_t* mac) {
    if (ap || (mac[0] & 0x01)) {
        return false;
    }

    wifi_get_mac_address(reinterpret_cast<char*>(system_mac), BK_STATION);
    std::memcpy(system_mac, mac, 6);

    const auto mode = wifi_get_mode();
    if (mode) {
        wifi_mode(WIFI_MODE_NULL);
        wifi_mode(mode);
    }

    return true;
}

uint8_t wifi_channel() {
    return link_status() ? static_cast<uint8_t>(wifi().link.channel) : 0;
}

// the port adds the link-local address itself
bool wifi_ipv6(bool) {
    return true;
}

// the BDK gives no reason names (arch/beken/wifi_driver_beken.cpp names what it reports)
const char* wifi_reason_name(int) {
    return "UNKNOWN";
}

bool wifi_scan_start(bool, bool, uint32_t max_ms_per_chan, uint8_t) {
    auto& state = scan();
    if (state.running) {
        if (state.timeout && (beken::millis() > state.timeout)) {
            state.running = false;
        } else {
            return true;
        }
    }

    if (!(wifi_get_mode() & WIFI_MODE_STA)) {
        wifi_mode(static_cast<wifi_mode_t>(wifi_get_mode() | WIFI_MODE_STA));
    }

    state.records.clear();
    state.running = true;
    state.timeout = beken::millis() + max_ms_per_chan * 20 + 1000;
    mhdr_scanu_reg_cb(scan_handler, nullptr);
    bk_wlan_start_scan();
    return true;
}

int16_t wifi_scan_count() {
    const auto& state = scan();
    if (state.running) {
        return WIFI_SCAN_RUNNING;
    }

    return static_cast<int16_t>(state.records.size());
}

bool wifi_scan_record(uint16_t index, WifiInfo& out) {
    const auto& state = scan();
    if (state.running || (index >= state.records.size())) {
        return false;
    }

    out = state.records[index];
    return true;
}

void wifi_scan_delete() {
    auto& state = scan();
    if (!state.running) {
        state.records.clear();
        state.records.shrink_to_fit();
    }
}

bool wifi_ap_config(uint32_t ip, uint32_t gateway, uint32_t netmask) {
    auto& ap = wifi().ap;
    if (!ip) {
        ip = gateway = ipaddr_addr("192.168.43.1");
        netmask = ipaddr_addr("255.255.255.0");
    }

    address_text(ap.local_ip_addr, sizeof(ap.local_ip_addr), ip);
    address_text(ap.net_mask, sizeof(ap.net_mask), netmask);
    address_text(ap.gateway_ip_addr, sizeof(ap.gateway_ip_addr), gateway);

    if (uap_ip_is_start()) {
        uap_ip_down();
        ip_address_set(BK_SOFT_AP, ap.dhcp_mode,
            ap.local_ip_addr, ap.net_mask, ap.gateway_ip_addr, ap.dns_server_ip_addr);
        uap_ip_start();
    }

    return true;
}

bool wifi_ap_start(const char* ssid, const char* passphrase, uint8_t channel, bool hidden, uint8_t max_connection) {
    if (!ssid || !ssid[0] || (std::strlen(ssid) > 32)) {
        return false;
    }

    if (!(wifi_get_mode() & WIFI_MODE_AP)) {
        wifi_mode(static_cast<wifi_mode_t>(wifi_get_mode() | WIFI_MODE_AP));
    }

    auto& ap = wifi().ap;
    std::memset(g_ap_param_ptr->key, '\0', 65);
    std::memset(ap.wifi_ssid, 0, sizeof(ap.wifi_ssid));
    std::strncpy(ap.wifi_ssid, ssid, sizeof(ap.wifi_ssid) - 1);
    if (passphrase && passphrase[0]) {
        std::strncpy(ap.wifi_key, passphrase, sizeof(ap.wifi_key) - 1);
        ap.security = BK_SECURITY_TYPE_WPA2_MIXED;
    } else {
        ap.wifi_key[0] = '\0';
        ap.security = BK_SECURITY_TYPE_NONE;
    }

    ap.channel = channel;
    ap.ssid_hidden = hidden;
    ap.max_con = max_connection;
    ap.dhcp_mode = DHCP_SERVER;
    ap.wifi_retry_interval = 100;
    if (!ap.local_ip_addr[0]) {
        wifi_ap_config(0, 0, 0);
    }

    return bk_wlan_start_ap_adv(&ap) == 0;
}

bool wifi_ap_stop() {
    if (!(wifi_get_mode() & WIFI_MODE_AP)) {
        return true;
    }

    return bk_wlan_stop(BK_SOFT_AP) == kNoErr;
}

uint32_t wifi_ap_ip() {
    return ip_status(BK_SOFT_AP) ? address(wifi().ip.ip) : 0;
}

bool wifi_ap_ssid(uint8_t (&out)[33]) {
    if (!uap_ip_is_start()) {
        std::memset(out, 0, sizeof(out));
        return false;
    }

    copy_ssid(out, wifi().ap.wifi_ssid);
    return true;
}

// the BDK does not count them
uint8_t wifi_ap_stations() {
    return 0;
}

// --- system ----------------------------------------------------------------------------------------------------------

uint32_t heap_free() {
    return beken::heap_free();
}

uint32_t heap_max_alloc() {
    return beken::heap_max_alloc();
}

uint32_t flash_size() {
    return beken::flash_size();
}

// not known on the BK72xx
uint32_t flash_mode() {
    return 0xFF;
}

uint32_t flash_speed() {
    return 0;
}

uint32_t cycle_count() {
    return beken::cpu_cycle_count();
}

uint32_t cpu_mhz() {
    return beken::cpu_mhz();
}

const char* chip_model() {
    return beken::cpu_model();
}

uint8_t chip_revision() {
    return 0;
}

void restart() {
    beken::reboot();
}

// code and initialized data as linked (the flash region starts at 0x10000)
uint32_t sketch_size() {
    constexpr uint32_t FlashOrigin { 0x10000 };
    const auto data = static_cast<uint32_t>(&_data_ram_end - &_data_ram_begin);
    return reinterpret_cast<uint32_t>(&_data_flash_begin) + data - FlashOrigin;
}

// the 'download' partition, the RBL of the next head goes there (ota_beken.cpp)
uint32_t sketch_space() {
    return FLASH_DOWNLOAD_LENGTH;
}

const char* sdk_version() {
    return beken::bdk_version();
}

// --- time ----------------------------------------------------------------------------------------------------------

uint64_t micros() {
    return beken::micros64();
}

void delay(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void delay_us(uint32_t us) {
    beken::delay_us(us);
}

void yield() {
    vTaskDelay(1);
    taskYIELD();
}

// between two loop() passes: the other tasks and the watchdog
void loop_pass() {
    vTaskDelay(1);
    taskYIELD();
    beken::wdt_feed();
}

// single core, the ARM968 port masks IRQ & FIQ; the flag of the outermost call is the one restored
namespace {

int irq_depth { 0 };
int irq_flag { 0 };

} // namespace

void irq_disable() {
    const int flag = port_disable_interrupts_flag();
    if (!irq_depth++) {
        irq_flag = flag;
    }
}

void irq_enable() {
    if (irq_depth && !--irq_depth) {
        port_enable_interrupts_flag(irq_flag);
    }
}

// --- GPIO ----------------------------------------------------------------------------------------------------------

namespace {

// the pads of the BK7231N QFN32 module (the Tuya CB/WB modules)
constexpr uint32_t ValidPins {
    (1UL << 0) | (1UL << 1) | (1UL << 6) | (1UL << 7) | (1UL << 8) | (1UL << 9) | (1UL << 10) | (1UL << 11)
    | (1UL << 14) | (1UL << 15) | (1UL << 16) | (1UL << 17) | (1UL << 20) | (1UL << 21) | (1UL << 22) | (1UL << 23)
    | (1UL << 24) | (1UL << 26) | (1UL << 28) };

constexpr uint8_t PinCount { 32 };

struct PinState {
    PinHandler handler;
    void* arg;
    uint8_t edge;
    bool change;
};

PinState pins[PinCount] {};

// a pin that was a PWM output (the light_pwm module, chip::pwm_bus) becomes a plain GPIO again
void pwm_release(uint8_t pin) {
    if (pwm_bus && pwm_bus->stop) {
        pwm_bus->stop(pin);
    }
}

} // namespace

bool pin_valid(uint8_t pin) {
    return (pin < PinCount) && (ValidPins & (1UL << pin));
}

void pin_mode(uint8_t pin, uint8_t mode) {
    if (!pin_valid(pin)) {
        return;
    }

    pwm_release(pin);
    switch (mode) {
    case pin::Input:
        gpio_config(pin, GMODE_INPUT);
        break;
    case pin::Output:
        gpio_config(pin, GMODE_OUTPUT);
        break;
    case pin::InputPullup:
        gpio_config(pin, GMODE_INPUT_PULLUP);
        break;
    case pin::InputPulldown:
        gpio_config(pin, GMODE_INPUT_PULLDOWN);
        break;
    case pin::OutputOpenDrain:
        gpio_config(pin, GMODE_SET_HIGH_IMPENDANCE);
        break;
    default:
        break;
    }
}

void pin_write(uint8_t pin, bool level) {
    if (pin_valid(pin)) {
        gpio_output(pin, level ? 1 : 0);
    }
}

bool pin_read(uint8_t pin) {
    return pin_valid(pin) && gpio_input(pin);
}

namespace {

void pin_isr(unsigned char gpio) {
    if (gpio >= PinCount) {
        return;
    }

    auto& state = pins[gpio];
    if (!state.handler) {
        return;
    }

    state.handler(state.arg);

    // CHANGE: the other edge next
    if (state.change) {
        state.edge = (state.edge == edge::Rising) ? edge::Falling : edge::Rising;
        gpio_int_enable(gpio,
            (state.edge == edge::Rising) ? GPIO_INT_LEVEL_RISING : GPIO_INT_LEVEL_FALLING, pin_isr);
    }
}

} // namespace

bool pin_attach(uint8_t pin, PinHandler handler, void* arg, uint8_t mode) {
    if (!pin_valid(pin)) {
        return false;
    }

    pwm_release(pin);

    auto& state = pins[pin];
    state.handler = handler;
    state.arg = arg;
    state.change = false;

    uint32_t event = 0;
    switch (mode) {
    case edge::Low:
        event = GPIO_INT_LEVEL_LOW;
        break;
    case edge::High:
        event = GPIO_INT_LEVEL_HIGH;
        break;
    case edge::Falling:
        event = GPIO_INT_LEVEL_FALLING;
        break;
    case edge::Rising:
        event = GPIO_INT_LEVEL_RISING;
        break;
    case edge::Change:
        if (gpio_input(pin)) {
            event = GPIO_INT_LEVEL_FALLING;
            mode = edge::Falling;
        } else {
            event = GPIO_INT_LEVEL_RISING;
            mode = edge::Rising;
        }
        state.change = true;
        break;
    default:
        return false;
    }

    state.edge = mode;
    gpio_int_enable(pin, event, pin_isr);
    return true;
}

void pin_detach(uint8_t pin) {
    if (!pin_valid(pin)) {
        return;
    }

    gpio_int_disable(pin);
    pins[pin].handler = nullptr;
    pins[pin].arg = nullptr;
}

// --- ADC: the SARADC, one channel (P23 = ADC3) ---------------------------------------------------------------------

namespace {

int adc_channel(uint8_t pin) {
    return (pin == 23) ? 3 : -1;
}

} // namespace

// in 10 bits of the 2.4 V range, as before
int adc_read(uint8_t pin) {
    const int channel = adc_channel(pin);
    if (channel < 0) {
        return 0;
    }

    static uint16_t data[1];

    saradc_desc_t adc;
    saradc_config_param_init(&adc);
    adc.channel = channel;
    adc.mode = ADC_CONFIG_MODE_CONTINUE;
    adc.pData = data;
    adc.data_buff_size = 1;

    UINT32 status;
    const auto handle = ddev_open(SARADC_DEV_NAME, &status, reinterpret_cast<uint32_t>(&adc));
    if (handle == DD_HANDLE_UNVALID) {
        return 0;
    }

    if (status != SARADC_SUCCESS) {
        ddev_close(handle);
        return 0;
    }

    for (int wait = 0; (!adc.has_data || (adc.current_sample_data_cnt < 1)) && (wait < 100); ++wait) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    uint8_t run_stop = 0;
    ddev_control(handle, SARADC_CMD_RUN_OR_STOP_ADC, &run_stop);
    ddev_close(handle);

    // 2048 = 2400 mV
    const uint32_t mv = static_cast<uint32_t>(data[0]) * 2400 / 2048;
    return static_cast<int>(std::min<uint32_t>(mv, 2400) * 1023 / 2400);
}

uint8_t adc_bits() {
    return 10;
}

// --- UART: UART1 is P11 (TX) & P10 (RX), the console and the flashing pads; UART2 is P0 (TX) & P1 (RX) -----------

namespace {

constexpr size_t UartBuffer { 256 };

// one ring buffer per port, filled by the BDK's receive callback (interrupt), read by the loop task
struct Uart {
    bool open;
    uint16_t config;
    uint8_t buffer[UartBuffer];
    volatile size_t head;
    volatile size_t tail;
};

Uart uarts[3] {};

bool uart_valid(int port) {
    return (port >= 1) && (port <= 2) && uarts[port].open;
}

void uart_receive(int uport, void* param) {
    auto* uart = static_cast<Uart*>(param);
    int value;
    while ((value = uart_read_byte(uport)) != -1) {
        const size_t next = (uart->head + 1) % UartBuffer;
        if (next != uart->tail) {
            uart->buffer[uart->head] = static_cast<uint8_t>(value);
            uart->head = next;
        }
    }
}

void uart_configure(int port, uint32_t baud, uint16_t config) {
    // the SERIAL_* numbers of ArduinoCore-API (chip::serial::): data 0x100..0x400, parity 0x1..0x3, stop 0x10 / 0x30
    bk_uart_config_t cfg{};
    cfg.baud_rate = baud;
    cfg.data_width = static_cast<uart_data_width_t>(((config & 0xF00) >> 8) - 1);
    cfg.parity = static_cast<uart_parity_t>(3 - (config & 0xF));
    cfg.stop_bits = static_cast<uart_stop_bits_t>((config & 0xF0) == 0x30);
    cfg.flow_control = FLOW_CTRL_DISABLED;

    if (port == 1) {
        uart1_init();
    } else {
        uart2_init();
    }

    uart_hw_set_change(static_cast<uint8_t>(port - 1), &cfg);
}

} // namespace

int uart_open(uint32_t baud, uint16_t config, uint8_t rx, uint8_t tx, bool, size_t) {
    const auto pins = [&](uint8_t port_tx, uint8_t port_rx) {
        return ((tx == port_tx) || (tx == NoPin))
            && ((rx == port_rx) || (rx == NoPin));
    };

    const int port =
        pins(11, 10) ? 1 :
        pins(0, 1) ? 2 : -1;
    if (port < 0) {
        return -1;
    }

    uart_close(port);

    auto& uart = uarts[port];
    uart.head = uart.tail = 0;
    uart.config = config;
    uart_configure(port, baud, config);
    if (rx != NoPin) {
        uart_rx_callback_set(port - 1, reinterpret_cast<uart_callback>(uart_receive), &uart);
    }

    uart.open = true;
    return port;
}

void uart_close(int port) {
    if (!uart_valid(port)) {
        return;
    }

    uart_rx_callback_set(port - 1, nullptr, nullptr);
    // the console UART stays on for the BDK's own log
    if ((port - 1) != uart_print_port) {
        if (port == 1) {
            uart1_exit();
        } else {
            uart2_exit();
        }
    }

    uarts[port].open = false;
}

void uart_baud(int port, uint32_t baud) {
    if (uart_valid(port)) {
        uart_wait_tx_over();
        uart_configure(port, baud, uarts[port].config);
    }
}

size_t uart_available(int port) {
    if (!uart_valid(port)) {
        return 0;
    }

    const auto& uart = uarts[port];
    return (uart.head + UartBuffer - uart.tail) % UartBuffer;
}

size_t uart_read(int port, uint8_t* out, size_t size) {
    if (!uart_valid(port)) {
        return 0;
    }

    auto& uart = uarts[port];
    size_t done = 0;
    while ((done < size) && (uart.tail != uart.head)) {
        out[done++] = uart.buffer[uart.tail];
        uart.tail = (uart.tail + 1) % UartBuffer;
    }

    return done;
}

int uart_peek(int port) {
    if (!uart_valid(port)) {
        return -1;
    }

    const auto& uart = uarts[port];
    return (uart.tail != uart.head) ? uart.buffer[uart.tail] : -1;
}

// bk_send_byte() waits for the FIFO itself
size_t uart_write_space(int port) {
    return uart_valid(port) ? 128 : 0;
}

size_t uart_write(int port, const uint8_t* data, size_t size) {
    if (!uart_valid(port)) {
        return 0;
    }

    for (size_t index = 0; index < size; ++index) {
        bk_send_byte(static_cast<uint8_t>(port - 1), data[index]);
    }

    return size;
}

void uart_flush(int port) {
    if (uart_valid(port)) {
        uart_wait_tx_over();
    }
}

} // namespace chip
} // namespace core
} // namespace espurna

// --- the start -----------------------------------------------------------------------------------------------------

namespace {

void main_task(void*) {
    espurna::core::run();
}

} // namespace

// from entry_main() of the SDK glue (fixups/arch_main.c), the drivers and the flash are up: the console UART, the C++ constructors,
// the loop task (8 KiB, as LibreTiny's "main"), the scheduler
// the entry the SDK's glue calls after the BDK's init (framework-beken-bdk glue/fixups/arch_main.c entry_main)
extern "C" void app_main(void) {
    uart_print_port = UART1_PORT;
    uart1_init();

    __libc_init_array();

    beken_thread_t thread;
    rtos_create_thread(&thread, THD_APPLICATION_PRIORITY, "main",
        reinterpret_cast<beken_thread_function_t>(main_task), 8192, nullptr);
    vTaskStartScheduler();
}
