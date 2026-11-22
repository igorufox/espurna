/*

ESP8266 on ESP8266_RTOS_SDK 3.4: the calls of the chip's SDK (arch/rtos/chip_sdk.h, wifi_sdk.h), the loop task and
app_main(). No Arduino. The same as arch/esp32/core_esp32.cpp on ESP-IDF, with
the SDK 3.4 drivers: GPIO with the ISR service, the UART driver (UART0 on its pins or swapped, UART1 TX only) and the
TOUT ADC (the I2C, SPI and PWM drivers are in the modules that use them: arch/soft/i2c_soft.cpp, spi_esp8266.cpp,
pwm_esp8266.cpp); the Wi-Fi of the SDK with tcpip_adapter and the default event loop

*/

#include <chip_sdk.h>
#include "rbl_esp8266.h"

#include <freertos/FreeRTOS.h>

#include <driver/adc.h>
#include <driver/gpio.h>
#include <driver/uart.h>

#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <rom/ets_sys.h>
#include <esp8266/pin_mux_register.h>
#include <esp_private/esp_system_internal.h>
#include <spi_flash.h>
#include <nvs.h>
#include <tcpip_adapter.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string.h>

// esp_restart() of libcore piece by piece (chip::restart()): the closed library's and the ROM's calls
extern "C" {
void pm_goto_rf_on(void);
void clockgate_watchdog(int);
void user_uart_wait_tx_fifo_empty(uint32_t, uint32_t);
void sleep_reset_analog_rtcreg_8266(void);
void bbpll_cal(int);
void rom_i2c_writeReg(uint8_t block, uint8_t host_id, uint8_t reg_add, uint8_t data);
void __esp_soc_restart(uint32_t) __attribute__((noreturn));

// the cycles the port's tick added up (freertos/port/esp8266/port.c), chip::cycle_count()
extern uint64_t g_esp_os_cpu_clk;
}

// the loop task (arch/rtos/tasks_chip.h)
TaskHandle_t loopTaskHandle { nullptr };

namespace espurna {
namespace core {
namespace esp8266 {
namespace {

// -----------------------------------------------------------------------------
// GPIO interrupts: the ISR service of the SDK
// -----------------------------------------------------------------------------

bool isr_service { false };

bool isr_install() {
    if (!isr_service) {
        const auto err = gpio_install_isr_service(0);
        isr_service = (err == ESP_OK) || (err == ESP_ERR_INVALID_STATE);
    }

    return isr_service;
}

gpio_int_type_t interrupt_type(uint8_t edge) {
    switch (edge) {
    case chip::edge::Low:
        return GPIO_INTR_LOW_LEVEL;
    case chip::edge::High:
        return GPIO_INTR_HIGH_LEVEL;
    case chip::edge::Change:
        return GPIO_INTR_ANYEDGE;
    case chip::edge::Falling:
        return GPIO_INTR_NEGEDGE;
    case chip::edge::Rising:
        return GPIO_INTR_POSEDGE;
    }

    return GPIO_INTR_DISABLE;
}

// -----------------------------------------------------------------------------
// ADC: the TOUT pad
// -----------------------------------------------------------------------------

bool adc_started { false };

// -----------------------------------------------------------------------------
// UART: UART0 (the console, its own pins or swapped to GPIO15 / GPIO13) and UART1 (TX on GPIO2)
// -----------------------------------------------------------------------------

struct Uart {
    bool open { false };
    int peeked { -1 };
};

Uart uarts[UART_NUM_MAX] {};

uart_word_length_t data_bits(uint16_t config) {
    switch (config & chip::serial::DataMask) {
    case chip::serial::Data5:
        return UART_DATA_5_BITS;
    case chip::serial::Data6:
        return UART_DATA_6_BITS;
    case chip::serial::Data7:
        return UART_DATA_7_BITS;
    }

    return UART_DATA_8_BITS;
}

uart_parity_t parity(uint16_t config) {
    switch (config & chip::serial::ParityMask) {
    case chip::serial::ParityEven:
        return UART_PARITY_EVEN;
    case chip::serial::ParityOdd:
        return UART_PARITY_ODD;
    }

    return UART_PARITY_DISABLE;
}

uart_stop_bits_t stop_bits(uint16_t config) {
    switch (config & chip::serial::StopBitMask) {
    case chip::serial::StopBit15:
        return UART_STOP_BITS_1_5;
    case chip::serial::StopBit2:
        return UART_STOP_BITS_2;
    }

    return UART_STOP_BITS_1;
}

// -----------------------------------------------------------------------------
// OTA
// -----------------------------------------------------------------------------

esp_ota_handle_t ota_handle { 0 };
const esp_partition_t* ota_partition { nullptr };

// -----------------------------------------------------------------------------
// Wi-Fi
// -----------------------------------------------------------------------------

namespace wifi {

bool inited { false };
bool started { false };
bool ipv6 { false };

char hostname[64] {};

// the scan: running, then its records (owned here, taken in the event task)
enum class Scan {
    Idle,
    Running,
    Done,
    Failed,
};

Scan scan { Scan::Idle };
wifi_ap_record_t* records { nullptr };
uint16_t count { 0 };

// the Wi-Fi library reports OPEN for an RSN it does not parse (foxy: FT-SAE / FT-PSK of 802.11r next to SAE and
// WPA-PSK, WebUI scan 11-18: every foxy AP "OPEN"; ESPurna takes OPEN for a network without a password): a cipher
// means it is not open - by the cipher, WEP or WPA2
wifi_auth_mode_t authmode(const wifi_ap_record_t& record) {
    if ((record.authmode != WIFI_AUTH_OPEN) || (record.pairwise_cipher == WIFI_CIPHER_TYPE_NONE)) {
        return record.authmode;
    }

    return ((record.pairwise_cipher == WIFI_CIPHER_TYPE_WEP40) || (record.pairwise_cipher == WIFI_CIPHER_TYPE_WEP104))
        ? WIFI_AUTH_WEP
        : WIFI_AUTH_WPA2_PSK;
}

void copy_ssid(uint8_t (&out)[33], const uint8_t* ssid, size_t size) {
    memset(out, 0, sizeof(out));
    memcpy(out, ssid, (size < 32) ? size : 32);
}

void scan_done(const wifi_event_sta_scan_done_t& done) {
    uint16_t number { 0 };
    esp_wifi_scan_get_ap_num(&number);

    wifi_ap_record_t* list { nullptr };
    if (number) {
        list = static_cast<wifi_ap_record_t*>(calloc(number, sizeof(wifi_ap_record_t)));
        if (!list || (esp_wifi_scan_get_ap_records(&number, list) != ESP_OK)) {
            free(list);
            list = nullptr;
            number = 0;
        }
    }

    portENTER_CRITICAL();
    free(records);
    records = list;
    count = number;
    scan = (done.status == 0) ? Scan::Done : Scan::Failed;
    portEXIT_CRITICAL();
}

// the handler of ESPurna's driver (wifi_on_event), called in the event task of the SDK
chip::WifiEventHandler handler { nullptr };

void event(void*, esp_event_base_t base, int32_t id, void* data) {
    chip::WifiEventInfo info{};
    chip::WifiEvent out;

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_SCAN_DONE:
            scan_done(*static_cast<wifi_event_sta_scan_done_t*>(data));
            out = chip::WifiEvent::ScanDone;
            break;

        case WIFI_EVENT_STA_CONNECTED:
            memcpy(info.bssid, static_cast<wifi_event_sta_connected_t*>(data)->bssid, sizeof(info.bssid));
            out = chip::WifiEvent::StaConnected;
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            const auto& disconnected = *static_cast<wifi_event_sta_disconnected_t*>(data);
            memcpy(info.bssid, disconnected.bssid, sizeof(info.bssid));
            info.reason = disconnected.reason;
            out = chip::WifiEvent::StaDisconnected;
            break;
        }

        default:
            return;
        }
    } else if ((base == IP_EVENT) && (id == IP_EVENT_STA_GOT_IP)) {
        out = chip::WifiEvent::StaGotIp;
    } else {
        return;
    }

    if (handler) {
        handler(out, info);
    }
}

// tcpip_adapter, the default event loop and the driver: once, on the first mode other than NULL
bool init() {
    if (inited) {
        return true;
    }

    tcpip_adapter_init();

    const auto loop = esp_event_loop_create_default();
    if ((loop != ESP_OK) && (loop != ESP_ERR_INVALID_STATE)) {
        return false;
    }

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&config) != ESP_OK) {
        return false;
    }

    // nothing in flash: the settings of ESPurna decide every start
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event, nullptr);
    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, event, nullptr);

    if (hostname[0]) {
        tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, hostname);
    }

    inited = true;
    return true;
}

} // namespace wifi

} // namespace
} // namespace esp8266

// -----------------------------------------------------------------------------
// The layer
// -----------------------------------------------------------------------------

namespace chip {

using namespace esp8266;

uint64_t micros() {
    return static_cast<uint64_t>(esp_timer_get_time());
}

// at least what was asked, rounded up to the tick (10 ms at CONFIG_FREERTOS_HZ 100): a shorter delay than one tick
// was vTaskDelay(0), the loop task never slept and the idle task starved (the task watchdog fired, 11-18 first run)
void delay(uint32_t ms) {
    vTaskDelay(ms ? ((ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS) : 0);
}

void delay_us(uint32_t us) {
    ets_delay_us(us);
}

void yield() {
    taskYIELD();
}

// the hardware watchdog of the SDK is fed by its idle task, the loop task by the stall watcher (system_esp8266.cpp)
void loop_pass() {
}

void irq_disable() {
    portENTER_CRITICAL();
}

void irq_enable() {
    portEXIT_CRITICAL();
}

bool pin_valid(uint8_t pin) {
    return GPIO_IS_VALID_GPIO(pin);
}

void pin_mode(uint8_t pin, uint8_t mode) {
    if (!GPIO_IS_VALID_GPIO(pin)) {
        return;
    }

    gpio_config_t config{};
    config.pin_bit_mask = uint32_t{1} << pin;
    config.intr_type = GPIO_INTR_DISABLE;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;

    switch (mode) {
    case pin::Input:
        config.mode = GPIO_MODE_INPUT;
        break;
    case pin::InputPullup:
        config.mode = GPIO_MODE_INPUT;
        config.pull_up_en = GPIO_PULLUP_ENABLE;
        break;
    case pin::InputPulldown:
        config.mode = GPIO_MODE_INPUT;
        config.pull_down_en = GPIO_PULLDOWN_ENABLE;
        break;
    case pin::Output:
        config.mode = GPIO_MODE_OUTPUT;
        break;
    case pin::OutputOpenDrain:
        config.mode = GPIO_MODE_OUTPUT_OD;
        break;
    }

    gpio_config(&config);
}

void pin_write(uint8_t pin, bool level) {
    gpio_set_level(static_cast<gpio_num_t>(pin), level ? 1 : 0);
}

// the input register also reads what an output drives
bool pin_read(uint8_t pin) {
    return gpio_get_level(static_cast<gpio_num_t>(pin)) != 0;
}

bool pin_attach(uint8_t pin, PinHandler handler, void* arg, uint8_t edge) {
    if ((pin > 15) || !isr_install()) {
        return false;
    }

    const auto num = static_cast<gpio_num_t>(pin);
    gpio_isr_handler_remove(num);
    gpio_set_intr_type(num, esp8266::interrupt_type(edge));
    return gpio_isr_handler_add(num, handler, arg) == ESP_OK;
}

void pin_detach(uint8_t pin) {
    if ((pin > 15) || !isr_service) {
        return;
    }

    const auto num = static_cast<gpio_num_t>(pin);
    gpio_set_intr_type(num, GPIO_INTR_DISABLE);
    gpio_isr_handler_remove(num);
}

// the TOUT pad (A0), 0..1023
int adc_read(uint8_t) {
    if (!adc_started) {
        adc_config_t config{};
        config.mode = ADC_READ_TOUT_MODE;
        config.clk_div = 8;
        adc_started = (adc_init(&config) == ESP_OK);
    }

    uint16_t out { 0 };
    if (!adc_started || (::adc_read(&out) != ESP_OK)) {
        return 0;
    }

    return out;
}

uint8_t adc_bits() {
    return 10;
}

int uart_open(uint32_t baud, uint16_t config, uint8_t rx, uint8_t tx, bool invert, size_t rx_buffer) {
    const bool console = ((rx == NoPin) && (tx == NoPin))
        || (((rx == 3) || (rx == NoPin)) && ((tx == 1) || (tx == NoPin)));
    const bool swapped = (((rx == 13) || (rx == NoPin)) && ((tx == 15) || (tx == NoPin)))
        && ((rx != NoPin) || (tx != NoPin));
    const uart_port_t port = (console || swapped) ? UART_NUM_0 : UART_NUM_1;

    if (uarts[port].open) {
        uart_close(port);
    }

    uart_config_t settings{};
    settings.baud_rate = static_cast<int>(baud);
    settings.data_bits = data_bits(config);
    settings.parity = parity(config);
    settings.stop_bits = stop_bits(config);
    settings.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_param_config(port, &settings);

    // the driver wants more than its FIFO (128 bytes) for RX; UART1 has no RX
    const int rx_size = (port == UART_NUM_1)
        ? 0
        : static_cast<int>((rx_buffer > 256) ? rx_buffer : 256);
    if (uart_driver_install(port, rx_size, 256, 0, nullptr, 0) != ESP_OK) {
        return -1;
    }

    if (port == UART_NUM_0) {
        if (swapped) {
            uart_enable_swap();
        } else {
            uart_disable_swap();
        }
    }

    if (invert) {
        uart_set_line_inverse(port, UART_INVERSE_RXD | UART_INVERSE_TXD);
    }

    uarts[port].open = true;
    uarts[port].peeked = -1;

    return static_cast<int>(port);
}

void uart_close(int port) {
    if ((port < 0) || (port >= UART_NUM_MAX) || !uarts[port].open) {
        return;
    }

    uart_driver_delete(static_cast<uart_port_t>(port));
    uarts[port] = Uart{};
}

void uart_baud(int port, uint32_t baud) {
    uart_wait_tx_done(static_cast<uart_port_t>(port), portMAX_DELAY);
    uart_set_baudrate(static_cast<uart_port_t>(port), baud);
}

size_t uart_available(int port) {
    size_t out { 0 };
    uart_get_buffered_data_len(static_cast<uart_port_t>(port), &out);
    return out + ((uarts[port].peeked >= 0) ? 1 : 0);
}

size_t uart_read(int port, uint8_t* out, size_t size) {
    if (!size) {
        return 0;
    }

    size_t done { 0 };
    if (uarts[port].peeked >= 0) {
        out[done++] = static_cast<uint8_t>(uarts[port].peeked);
        uarts[port].peeked = -1;
    }

    if (done < size) {
        const auto len = uart_read_bytes(static_cast<uart_port_t>(port), out + done, size - done, 0);
        if (len > 0) {
            done += static_cast<size_t>(len);
        }
    }

    return done;
}

int uart_peek(int port) {
    if (uarts[port].peeked < 0) {
        uint8_t value;
        if (uart_read_bytes(static_cast<uart_port_t>(port), &value, 1, 0) == 1) {
            uarts[port].peeked = value;
        }
    }

    return uarts[port].peeked;
}

// the driver's TX ring buffer does not tell its free space: what one write takes without waiting
size_t uart_write_space(int) {
    return 128;
}

size_t uart_write(int port, const uint8_t* data, size_t size) {
    const auto out = uart_write_bytes(static_cast<uart_port_t>(port), reinterpret_cast<const char*>(data), size);
    return (out > 0) ? static_cast<size_t>(out) : 0;
}

void uart_flush(int port) {
    uart_wait_tx_done(static_cast<uart_port_t>(port), portMAX_DELAY);
}

// the free heap of the SDK (MALLOC_CAP_32BIT: what malloc() takes from)
uint32_t heap_free() {
    return esp_get_free_heap_size();
}

// the largest block malloc() can take (MALLOC_CAP_32BIT, as the SDK's malloc): the walk of the SDK package's heap
// (heap_caps_get_largest_free_block, ESPurna's addition); before it the free heap counted as one block - WebUI
// "Usable heap" was always "Free heap", fragmentation 0% (owner 18.11: «подозрительно одинаково»)
uint32_t heap_max_alloc() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_32BIT);
}

uint32_t flash_size() {
    return spi_flash_get_chip_size();
}

namespace {

// the image header at 0x0: the flash mode and speed the board boots with
bool image_header(uint8_t (&out)[4]) {
    return spi_flash_read(0, out, sizeof(out)) == ESP_OK;
}

} // namespace

// QIO 0, QOUT 1, DIO 2, DOUT 3 (FlashMode_t of the cores)
uint32_t flash_mode() {
    uint8_t header[4] {};
    return image_header(header) ? header[2] : 0;
}

uint32_t flash_speed() {
    uint8_t header[4] {};
    if (!image_header(header)) {
        return 0;
    }

    switch (header[3] & 0xf) {
    case 0x0:
        return 40000000;
    case 0x1:
        return 26000000;
    case 0x2:
        return 20000000;
    case 0xf:
        return 80000000;
    }

    return 0;
}

// the length of the running app image as the bootloader reads it (image format of the SDK, esptool elf2image
// --version=3: an 8 byte header, the segments with their 8 byte headers, the checksum byte at the end of a 16 byte
// block); the module area starts behind it (modules_esp8266.cpp)
uint32_t sketch_size() {
    const auto* running = esp_ota_get_running_partition();
    if (!running) {
        return 0;
    }

    uint8_t header[8];
    if ((spi_flash_read(running->address, header, sizeof(header)) != ESP_OK) || (header[0] != 0xE9)) {
        return 0;
    }

    uint32_t pos = sizeof(header);
    for (uint8_t segment = 0; segment < header[1]; ++segment) {
        uint32_t segment_header[2];
        if (spi_flash_read(running->address + pos, segment_header, sizeof(segment_header)) != ESP_OK) {
            return 0;
        }

        pos += sizeof(segment_header) + segment_header[1];
        if (pos > running->size) {
            return 0;
        }
    }

    return (pos + 1 + 15) & ~uint32_t{15};
}

// what an update can take: the package of a new head (RBL, ota_esp8266.cpp) lies under its header at
// ESPURNA_RBL_HEADER_AT, above the running head (the modules off for it). No second app partition: the SDK's
// "next update partition" said 0 (WebUI STATUS "Free space 0 bytes", 18.11)
uint32_t sketch_space() {
    // 2 MB and more: the package goes to the top zone of the flash (rbl_esp8266.h, item 22 stage 2)
    if (spi_flash_get_chip_size() >= 0x200000u) {
        return ESPURNA_RBL_TOP_ZONE;
    }

    const uint32_t end = espurna_rbl_round(ESPURNA_RBL_HEAD_AT + sketch_size());
    return (ESPURNA_RBL_HEADER_AT > end) ? (ESPURNA_RBL_HEADER_AT - end) : 0;
}

// The port's tick sets CCOUNT back to 0 every tick and adds what it counted to g_esp_os_cpu_clk (freertos/port/esp8266
// port.c xPortSysTickHandle): CCOUNT alone is no clock (11-18: every WebSocket message timed out at once, the CpuClock
// of the queue and of the LED patterns). The cycles since the boot, as esp_timer_get_time() takes the microseconds;
// read again when a tick came in between
uint32_t cycle_count() {
    uint64_t before;
    uint32_t ccount;
    uint64_t after;
    do {
        before = *const_cast<volatile uint64_t*>(&g_esp_os_cpu_clk);
        __asm__ __volatile__("rsr %0, ccount" : "=a" (ccount));
        after = *const_cast<volatile uint64_t*>(&g_esp_os_cpu_clk);
    } while (before != after);

    return static_cast<uint32_t>(before + ccount);
}

uint32_t cpu_mhz() {
    return CONFIG_ESP8266_DEFAULT_CPU_FREQ_MHZ;
}

const char* sdk_version() {
    return esp_get_idf_version();
}

uint8_t chip_revision() {
    return 0;
}

const char* chip_model() {
    return "ESP8266EX";
}

// esp_restart() of the SDK without its first step (11-18, the COM3 board): it stops the station and the access point
// first, and wifi_softap_stop() of the Wi-Fi library broke the pending ready list of FreeRTOS-Kernel V11 (the MAC task
// woken by the Wi-Fi interrupt while the library resumed the scheduler): a LoadProhibited in xTaskResumeAll, then the
// watchdog.
//
// The restart itself is a reset of the chip by the hardware watchdog (as hardware_restart() of the SDK's panic.c), not
// esp_restart()'s jump to the ROM's reset vector: after that jump the SPI flash controller stays as the SDK set it, and
// flash writes the ROM loader and the bootloader then make did not land while answering OK (19.11 COM3: the head
// written over the wire and the RBL "installed" by the bootloader both left the previous head; regress-todo.md items
// 23, 31). The hint says what it was (reset_reason.c of the package maps OWDT_RESET by it)
void restart() {
    // leave the AP before the reset (a deauth, protected with PMF): without it foxy (SAE + PMF) kept the old association
    // and turned the first attempt after every restart away (status 30, ASSOC_FAIL 203, +5 s to every boot). Safe since
    // the Wi-Fi adapter of the SDK no longer suspends the scheduler under the library's interrupt lock (19.11)
    if (wifi_sta_disconnect()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    user_uart_wait_tx_fifo_empty(0, 500000);
    user_uart_wait_tx_fifo_empty(1, 500000);
    vPortETSIntrLock();
    esp_reset_reason_set_hint(ESP_RST_SW);

    // the analog registers and the PLL back to what the ROM expects, as esp_restart() does: a reset by the watchdog
    // keeps the RTC domain, and without these the bootloader printed at a wrong UART divider (19.11, garbled log)
    sleep_reset_analog_rtcreg_8266();
    rom_i2c_writeReg(103, 4, 1, 8);
    rom_i2c_writeReg(103, 4, 2, 129);
    bbpll_cal(1);

    volatile uint32_t* const wdt = reinterpret_cast<volatile uint32_t*>(0x60000900);
    wdt[0] &= ~uint32_t{1};                                 // WDT_CTL: off while it is set up
    wdt[1] = 1;                                             // WDT_OP: the shortest stage
    wdt[2] = 1;                                             // WDT_OP_ND
    wdt[0] = (wdt[0] & ~uint32_t{0x38}) | (7u << 3);        // RSTLEN
    wdt[0] = (wdt[0] & ~uint32_t{0x6}) | (0u << 1);         // RSPMOD: reset at once, no interrupt stage
    wdt[0] |= 1u;                                           // EN
    for (;;) {
    }
}

// --- the ROM's UART download mode without a GPIO0 jumper (regress-todo.md item 10) ---------------------------------
//
// After the reset handler of the ROM, as the ROM itself enters it with GPIO0 low: the cache off, the vectors and the
// stack of the ROM, UART0 attached, its baud rate detected, UartDwnLdProc() - the protocol of esptool (--before
// no-reset). Based on esp8266/Arduino cores/esp8266/reboot_uart_dwnld.cpp, Copyright (c) 2021 Timo Wischer, LGPL 2.1
// (reverse engineered from the ROM); the ROM addresses from eagle.rom.addr.v6.ld of the NONOS SDK.

extern "C" {

void Cache_Read_Disable();
void Wait_SPI_Idle(void* flashchip);

} // extern "C"

namespace {

// the ROM calls the SDK's esp8266.rom.ld does not name (eagle.rom.addr.v6.ld)
using RomVoid = void (*)();
using RomUartInit = void (*)(uint32_t);
using RomBaudDetect = int (*)(int, int);
using RomDivModify = void (*)(uint8_t, uint32_t);
using RomDownload = void (*)(uint8_t*, uint32_t, void (**)());
using RomBoot = int (*)();

const auto rom_uartAttach = reinterpret_cast<RomVoid>(0x4000383c);
const auto rom_Uart_Init = reinterpret_cast<RomUartInit>(0x40003a14);
const auto rom_ets_install_uart_printf = reinterpret_cast<RomVoid>(0x40002438);
const auto rom_uart_baudrate_detect = reinterpret_cast<RomBaudDetect>(0x40003924);
const auto rom_uart_div_modify = reinterpret_cast<RomDivModify>(0x400039d8);
const auto rom_UartDwnLdProc = reinterpret_cast<RomDownload>(0x40003368);
const auto rom_boot_from_flash = reinterpret_cast<RomBoot>(0x40001308);
const auto rom_ets_run = reinterpret_cast<RomVoid>(0x40000e04);
auto& rom_user_start_fptr = *reinterpret_cast<void (**)()>(0x3fffdcd0);

} // namespace

namespace {

// noinline: inlined into rom_loader() (flash) it ran on from the flash right after Cache_Read_Disable() - an
// IllegalInstruction, the board restarted instead of the ROM loader (RESET.ROM 20.11; the regression's "RESET.ROM and
// back" passed on that restart)
[[noreturn]] void IRAM_ATTR __attribute__((noinline)) uart_download() {
    Wait_SPI_Idle(reinterpret_cast<void*>(0x3fffc714));  // the ROM's flashchip record (g_rom_flashchip)
    Cache_Read_Disable();
    // the IRAM the cache took, back to the ROM's 64 KB (a hardware reset clears these, Cache_Read_Disable() does not)
    *reinterpret_cast<volatile uint32_t*>(0x3ff00024) &= ~uint32_t{(1u << 3) | (1u << 4)};

    __asm__ __volatile__("wsr.intenable %0" :: "r"(0));
    __asm__ __volatile__("wsr.litbase %0" :: "r"(0));
    __asm__ __volatile__("rsync");
    __asm__ __volatile__("wsr.vecbase %0" :: "r"(0x40000000));
    uint32_t state;
    __asm__ __volatile__("rsil %0, 1" : "=r"(state));
    __asm__ __volatile__("mov a1, %0" :: "r"(0x40000000));
    __asm__ __volatile__("wsr.ps %0" :: "r"(0x20));
    __asm__ __volatile__("rsync");

    // UART0 back on GPIO1 / GPIO3 for the ROM loader: the head or a module may have moved it (the swap to GPIO15 / GPIO13,
    // IOSWAP bit 2) or taken the pins, and rom_Uart_Init() sets neither (20.11: after RESET.ROM the ROM never answered
    // the sync of esptool on any baud rate, the board was lost until a power cycle)
    *reinterpret_cast<volatile uint32_t*>(0x3ff00028) &= ~uint32_t{1u << 2};
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_U0TXD);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, FUNC_U0RXD);

    rom_uartAttach();
    rom_Uart_Init(0);
    rom_ets_install_uart_printf();
    rom_uart_div_modify(0, static_cast<uint32_t>(rom_uart_baudrate_detect(0, 0)));
    rom_UartDwnLdProc(reinterpret_cast<uint8_t*>(0x3fffa000), 0x2000, &rom_user_start_fptr);

    if (!rom_user_start_fptr) {
        rom_boot_from_flash();
    }
    if (rom_user_start_fptr) {
        rom_user_start_fptr();
    }

    rom_ets_run();
    for (;;) {
    }
}

} // namespace

[[noreturn]] void rom_loader() {
    pm_goto_rf_on();
    clockgate_watchdog(0);
    *reinterpret_cast<volatile uint32_t*>(0x3ff00018) = 0xffff00ff;
    user_uart_wait_tx_fifo_empty(0, 500000);
    user_uart_wait_tx_fifo_empty(1, 500000);
    vPortETSIntrLock();
    // the hardware watchdog off: the ROM's loader never feeds it
    *reinterpret_cast<volatile uint32_t*>(0x60000900) &= ~uint32_t{1};
    *reinterpret_cast<volatile uint32_t*>(0x3ff00018) |= 0x7500;
    *reinterpret_cast<volatile uint32_t*>(0x3ff00018) &= ~uint32_t{0x7500};
    *reinterpret_cast<volatile uint32_t*>(0x60000a00 + 0x348) |= 0x2;
    *reinterpret_cast<volatile uint32_t*>(0x60000a00 + 0x348) &= ~uint32_t{0x2};
    uart_download();
}

const char* ota_begin(size_t size) {
    ota_abort();

    ota_partition = esp_ota_get_next_update_partition(nullptr);
    if (!ota_partition) {
        return "No OTA partition";
    }

    if ((size != OtaSizeUnknown) && (size > ota_partition->size)) {
        ota_partition = nullptr;
        return "Not enough space";
    }

    const auto image = (size == OtaSizeUnknown) ? OTA_SIZE_UNKNOWN : size;
    if (esp_ota_begin(ota_partition, image, &ota_handle) != ESP_OK) {
        ota_partition = nullptr;
        ota_handle = 0;
        return "Flash Erase Failed";
    }

    return nullptr;
}

const char* ota_write(const uint8_t* data, size_t size) {
    if (!ota_handle) {
        return "Not running";
    }

    const auto err = esp_ota_write(ota_handle, data, size);
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        return "Wrong Magic Byte";
    }

    return (err == ESP_OK) ? nullptr : "Flash Write Failed";
}

const char* ota_end() {
    if (!ota_handle) {
        return "Not running";
    }

    const auto handle = ota_handle;
    ota_handle = 0;

    if (esp_ota_end(handle) != ESP_OK) {
        ota_partition = nullptr;
        return "Could Not Activate The Firmware";
    }

    const auto err = esp_ota_set_boot_partition(ota_partition);
    ota_partition = nullptr;

    return (err == ESP_OK) ? nullptr : "Could Not Activate The Firmware";
}

// the SDK has no esp_ota_abort(): the next begin() erases again
void ota_abort() {
    ota_handle = 0;
    ota_partition = nullptr;
}

bool wifi_mode(wifi_mode_t mode) {
    if (mode == WIFI_MODE_NULL) {
        if (wifi::started) {
            esp_wifi_stop();
            wifi::started = false;
        }

        return true;
    }

    if (!wifi::init()) {
        return false;
    }

    // the AP goes down on the fly again (regress-todo 3): wifi_softap_stop() broke the lists of FreeRTOS-Kernel V11 while
    // its List_t had the mini end item - configUSE_MINI_LIST_ITEM 0 since 19.11 (GCC 14 strict aliasing, item 17)
    if (esp_wifi_set_mode(mode) != ESP_OK) {
        return false;
    }

    if (!wifi::started) {
        if (esp_wifi_start() != ESP_OK) {
            return false;
        }

        wifi::started = true;
    }

    return true;
}

wifi_mode_t wifi_get_mode() {
    if (!wifi::started) {
        return WIFI_MODE_NULL;
    }

    wifi_mode_t out { WIFI_MODE_NULL };
    esp_wifi_get_mode(&out);
    return out;
}

bool wifi_sleep(bool enabled) {
    return esp_wifi_set_ps(enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE) == ESP_OK;
}

// WPA2 at least when there is a passphrase (WPA3-SAE where the AP takes it, sdkconfig.defaults), PMF capable. The SDK
// picks the AP (owner 2026-11-18: no roaming scans of ESPurna's own): every channel is scanned and the strongest AP of
// the network is taken (a fast scan takes the first one found, the sort has nothing to sort then). No 802.11k/v
// (owner 2026-11-18: the boards stand still, 7.7 KB of wpa_supplicant for roaming): an AP that drops the station
// (usteer) gets the next attempt, which takes the strongest AP again
bool wifi_sta_begin(const char* ssid, const char* passphrase, uint8_t channel, const uint8_t* bssid) {
    wifi_config_t config{};
    strncpy(reinterpret_cast<char*>(config.sta.ssid), ssid, sizeof(config.sta.ssid));
    if (passphrase) {
        strncpy(reinterpret_cast<char*>(config.sta.password), passphrase, sizeof(config.sta.password));
    }

    // a known channel (the AP of the last connection, wifi.cpp last_ap; or the network's own channel setting): that
    // channel only, the first match - the scan of all 13 channels took 2.1 s of every connect (19.11)
    config.sta.scan_method = channel ? WIFI_FAST_SCAN : WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.threshold.authmode = (passphrase && *passphrase) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.channel = channel;
    if (bssid) {
        config.sta.bssid_set = true;
        memcpy(config.sta.bssid, bssid, sizeof(config.sta.bssid));
    }

    esp_wifi_disconnect();
    if (esp_wifi_set_config(ESP_IF_WIFI_STA, &config) != ESP_OK) {
        return false;
    }

    return esp_wifi_connect() == ESP_OK;
}

bool wifi_sta_disconnect() {
    if (!wifi::started) {
        return false;
    }

    return esp_wifi_disconnect() == ESP_OK;
}

bool wifi_sta_config(uint32_t ip, uint32_t gateway, uint32_t netmask, uint32_t dns1, uint32_t dns2) {
    tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_STA);

    tcpip_adapter_ip_info_t info{};
    info.ip.addr = ip;
    info.gw.addr = gateway;
    info.netmask.addr = netmask;
    tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_STA, &info);

    if (!ip) {
        return tcpip_adapter_dhcpc_start(TCPIP_ADAPTER_IF_STA) == ESP_OK;
    }

    const uint32_t servers[] {dns1, dns2};
    const tcpip_adapter_dns_type_t types[] {TCPIP_ADAPTER_DNS_MAIN, TCPIP_ADAPTER_DNS_BACKUP};
    for (size_t index = 0; index < 2; ++index) {
        if (servers[index]) {
            tcpip_adapter_dns_info_t dns{};
            IP_ADDR4(&dns.ip, servers[index] & 0xff, (servers[index] >> 8) & 0xff,
                (servers[index] >> 16) & 0xff, (servers[index] >> 24) & 0xff);
            tcpip_adapter_set_dns_info(TCPIP_ADAPTER_IF_STA, types[index], &dns);
        }
    }

    return true;
}

bool wifi_sta_address(uint32_t& ip, uint32_t& netmask, uint32_t& gateway) {
    tcpip_adapter_ip_info_t info{};
    if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &info) != ESP_OK) {
        return false;
    }

    ip = info.ip.addr;
    netmask = info.netmask.addr;
    gateway = info.gw.addr;
    return true;
}

bool wifi_sta_info(WifiInfo& out) {
    wifi_ap_record_t record{};
    if (!wifi::started || (esp_wifi_sta_get_ap_info(&record) != ESP_OK)) {
        return false;
    }

    wifi::copy_ssid(out.ssid, record.ssid, sizeof(record.ssid));
    memcpy(out.bssid, record.bssid, 6);
    out.channel = record.primary;
    out.rssi = record.rssi;
    out.authmode = wifi::authmode(record);
    return true;
}

bool wifi_hostname(const char* hostname) {
    strncpy(wifi::hostname, hostname ? hostname : "", sizeof(wifi::hostname) - 1);
    if (wifi::inited) {
        return tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, wifi::hostname) == ESP_OK;
    }

    return true;
}

const char* wifi_get_hostname() {
    return wifi::hostname;
}

bool wifi_mac(bool ap, uint8_t* out) {
    if (wifi::inited) {
        return esp_wifi_get_mac(ap ? ESP_IF_WIFI_AP : ESP_IF_WIFI_STA, out) == ESP_OK;
    }

    return esp_read_mac(out, ap ? ESP_MAC_WIFI_SOFTAP : ESP_MAC_WIFI_STA) == ESP_OK;
}

bool wifi_set_mac(bool ap, const uint8_t* mac) {
    if (!wifi::inited) {
        return false;
    }

    return esp_wifi_set_mac(ap ? ESP_IF_WIFI_AP : ESP_IF_WIFI_STA, mac) == ESP_OK;
}

uint8_t wifi_channel() {
    uint8_t primary { 0 };
    wifi_second_chan_t second;
    if (!wifi::started || (esp_wifi_get_channel(&primary, &second) != ESP_OK)) {
        return 0;
    }

    return primary;
}

// lwIP of the SDK is built without IPv6 (as the NONOS build's lwip2): nothing to enable
bool wifi_ipv6(bool enable) {
    wifi::ipv6 = false;
    return !enable;
}

const char* wifi_reason_name(int reason) {
    switch (reason) {
    case WIFI_REASON_UNSPECIFIED: return "UNSPECIFIED";
    case WIFI_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE: return "AUTH_LEAVE";
    case WIFI_REASON_ASSOC_EXPIRE: return "ASSOC_EXPIRE";
    case WIFI_REASON_ASSOC_TOOMANY: return "ASSOC_TOOMANY";
    case WIFI_REASON_NOT_AUTHED: return "NOT_AUTHED";
    case WIFI_REASON_NOT_ASSOCED: return "NOT_ASSOCED";
    case WIFI_REASON_ASSOC_LEAVE: return "ASSOC_LEAVE";
    case WIFI_REASON_ASSOC_NOT_AUTHED: return "ASSOC_NOT_AUTHED";
    case WIFI_REASON_DISASSOC_PWRCAP_BAD: return "DISASSOC_PWRCAP_BAD";
    case WIFI_REASON_DISASSOC_SUPCHAN_BAD: return "DISASSOC_SUPCHAN_BAD";
    case WIFI_REASON_IE_INVALID: return "IE_INVALID";
    case WIFI_REASON_MIC_FAILURE: return "MIC_FAILURE";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: return "GROUP_KEY_UPDATE_TIMEOUT";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS: return "IE_IN_4WAY_DIFFERS";
    case WIFI_REASON_GROUP_CIPHER_INVALID: return "GROUP_CIPHER_INVALID";
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID: return "PAIRWISE_CIPHER_INVALID";
    case WIFI_REASON_AKMP_INVALID: return "AKMP_INVALID";
    case WIFI_REASON_UNSUPP_RSN_IE_VERSION: return "UNSUPP_RSN_IE_VERSION";
    case WIFI_REASON_INVALID_RSN_IE_CAP: return "INVALID_RSN_IE_CAP";
    case WIFI_REASON_802_1X_AUTH_FAILED: return "802_1X_AUTH_FAILED";
    case WIFI_REASON_CIPHER_SUITE_REJECTED: return "CIPHER_SUITE_REJECTED";
    case WIFI_REASON_INVALID_PMKID: return "INVALID_PMKID";
    case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL: return "CONNECTION_FAIL";
    case WIFI_REASON_AP_TSF_RESET: return "AP_TSF_RESET";
    case WIFI_REASON_BASIC_RATE_NOT_SUPPORT: return "BASIC_RATE_NOT_SUPPORT";
    }

    return "UNKNOWN";
}

// active scan with a 100 ms minimum per channel, passive with the maximum
bool wifi_scan_start(bool show_hidden, bool passive, uint32_t max_ms_per_chan, uint8_t channel) {
    if (!wifi::started) {
        return false;
    }

    wifi_scan_config_t config{};
    config.channel = channel;
    config.show_hidden = show_hidden;
    if (passive) {
        config.scan_type = WIFI_SCAN_TYPE_PASSIVE;
        config.scan_time.passive = max_ms_per_chan;
    } else {
        config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        config.scan_time.active.min = 100;
        config.scan_time.active.max = max_ms_per_chan;
    }

    portENTER_CRITICAL();
    wifi::scan = wifi::Scan::Running;
    portEXIT_CRITICAL();

    if (esp_wifi_scan_start(&config, false) != ESP_OK) {
        portENTER_CRITICAL();
        wifi::scan = wifi::Scan::Failed;
        portEXIT_CRITICAL();
        return false;
    }

    return true;
}

int16_t wifi_scan_count() {
    portENTER_CRITICAL();
    const auto scan = wifi::scan;
    const auto count = wifi::count;
    portEXIT_CRITICAL();

    switch (scan) {
    case wifi::Scan::Running:
        return WIFI_SCAN_RUNNING;
    case wifi::Scan::Done:
        return static_cast<int16_t>(count);
    case wifi::Scan::Idle:
    case wifi::Scan::Failed:
        break;
    }

    return WIFI_SCAN_FAILED;
}

bool wifi_scan_record(uint16_t index, WifiInfo& out) {
    portENTER_CRITICAL();
    const bool valid = (wifi::scan == wifi::Scan::Done) && wifi::records && (index < wifi::count);
    wifi_ap_record_t record{};
    if (valid) {
        record = wifi::records[index];
    }
    portEXIT_CRITICAL();

    if (!valid) {
        return false;
    }

    wifi::copy_ssid(out.ssid, record.ssid, sizeof(record.ssid));
    memcpy(out.bssid, record.bssid, 6);
    out.channel = record.primary;
    out.rssi = record.rssi;
    out.authmode = wifi::authmode(record);
    return true;
}

void wifi_scan_delete() {
    portENTER_CRITICAL();
    auto* records = wifi::records;
    wifi::records = nullptr;
    wifi::count = 0;
    if (wifi::scan != wifi::Scan::Running) {
        wifi::scan = wifi::Scan::Idle;
    }
    portEXIT_CRITICAL();

    free(records);
}

bool wifi_ap_start(const char* ssid, const char* passphrase, uint8_t channel, bool hidden, uint8_t max_connection) {
    wifi_config_t config{};
    const auto ssid_len = strnlen(ssid, sizeof(config.ap.ssid));
    memcpy(config.ap.ssid, ssid, ssid_len);
    config.ap.ssid_len = static_cast<uint8_t>(ssid_len);
    config.ap.channel = channel ? channel : 1;
    config.ap.ssid_hidden = hidden ? 1 : 0;
    config.ap.max_connection = max_connection;
    config.ap.beacon_interval = 100;
    if (passphrase) {
        strncpy(reinterpret_cast<char*>(config.ap.password), passphrase, sizeof(config.ap.password));
        config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        config.ap.authmode = WIFI_AUTH_OPEN;
    }

    return esp_wifi_set_config(ESP_IF_WIFI_AP, &config) == ESP_OK;
}

// the address, DHCP leases after it, the AP itself offered as the DNS server (the captive portal answers there)
bool wifi_ap_config(uint32_t ip, uint32_t gateway, uint32_t netmask) {
    if (!wifi::init()) {
        return false;
    }

    tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP);

    tcpip_adapter_ip_info_t info{};
    info.ip.addr = ip;
    info.gw.addr = gateway;
    info.netmask.addr = netmask;
    if (tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &info) != ESP_OK) {
        return false;
    }

    uint8_t offer = 1; // OFFER_DNS
    tcpip_adapter_dhcps_option(TCPIP_ADAPTER_OP_SET, TCPIP_ADAPTER_DOMAIN_NAME_SERVER, &offer, sizeof(offer));

    return tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP) == ESP_OK;
}

// dropping the AP bit of the mode is what stops it (WiFiClass::softAPdisconnect(true))
bool wifi_ap_stop() {
    return true;
}

uint32_t wifi_ap_ip() {
    tcpip_adapter_ip_info_t info{};
    if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &info) != ESP_OK) {
        return 0;
    }

    return info.ip.addr;
}

bool wifi_ap_ssid(uint8_t (&out)[33]) {
    wifi_config_t config{};
    if (!wifi::started || (esp_wifi_get_config(ESP_IF_WIFI_AP, &config) != ESP_OK)) {
        return false;
    }

    wifi::copy_ssid(out, config.ap.ssid, config.ap.ssid_len ? config.ap.ssid_len : sizeof(config.ap.ssid));
    return true;
}

uint8_t wifi_ap_stations() {
    wifi_sta_list_t list{};
    if (!wifi::started || (esp_wifi_ap_get_sta_list(&list) != ESP_OK)) {
        return 0;
    }

    return static_cast<uint8_t>(list.num);
}

void wifi_on_event(WifiEventHandler handler) {
    esp8266::wifi::handler = handler;
}

} // namespace chip
} // namespace core
} // namespace espurna

// -----------------------------------------------------------------------------
// Start
// -----------------------------------------------------------------------------

namespace {

void loop_task(void*) {
    espurna::core::run();
}

} // namespace

// the loop task: 8 KB of stack (the ESP32 and Beken loop tasks have the same), priority 1 as on the other chips
// No NVS (sdkconfig.defaults): the closed Wi-Fi libraries (libcore misc_nvs, libnet80211 ieee80211_nvs), the SDK's phy_init and lwIP's
// dhcp_state still name these
// calls, and the linker took the whole NVS storage (12 KB) for them. Nothing opens NVS: every call says so
extern "C" {

esp_err_t nvs_open(const char*, nvs_open_mode_t, nvs_handle_t*) {
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

void nvs_close(nvs_handle_t) {
}

esp_err_t nvs_get_blob(nvs_handle_t, const char*, void*, size_t*) {
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void*, size_t) {
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_commit(nvs_handle_t) {
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_erase_key(nvs_handle_t, const char*) {
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_get_i8(nvs_handle_t, const char*, int8_t*) {
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_get_u8(nvs_handle_t, const char*, uint8_t*) {
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_get_u16(nvs_handle_t, const char*, uint16_t*) {
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_get_u32(nvs_handle_t, const char*, uint32_t*) {
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_set_i8(nvs_handle_t, const char*, int8_t) {
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_set_u8(nvs_handle_t, const char*, uint8_t) {
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_set_u16(nvs_handle_t, const char*, uint16_t) {
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

esp_err_t nvs_set_u32(nvs_handle_t, const char*, uint32_t) {
    return ESP_ERR_NVS_NOT_INITIALIZED;
}

} // extern "C"

// BearSSL's ESP8266 branch (core_arduino_chip.h): no second stack on the RTOS SDK, the loop task runs TLS on its own
extern "C" void stack_thunk_yield() {
}

// spi_flash.c of the SDK, not in its headers: the JEDEC id (manufacturer, type, capacity from the low byte up), and
// the ROM's flash chip record (esp8266/rom_functions.h, which redeclares the ROM calls above with C++ linkage)
extern "C" uint32_t spi_flash_get_id(void);

struct EspurnaRomFlashChip {
    uint32_t device_id;
    uint32_t chip_size;
    uint32_t block_size;
    uint32_t sector_size;
    uint32_t page_size;
    uint32_t status_mask;
};

extern "C" EspurnaRomFlashChip g_rom_flashchip;

namespace {

// The size of the flash is the image header's (the 1 MB layout, sdkconfig.defaults) and the SDK refuses every read,
// write and erase beyond it: on a bigger chip the real size from its JEDEC id (capacity byte: 2^n bytes), the flash
// above 1 MB for the modules' data and the RBL of a new head (regress-todo.md item 22, owner 18.11: «ДА на nonos, а мы
// сейчас на RTOS»). The CPU still runs code from the first MB alone (the cache window)
void flash_size_from_id() {
    const uint32_t id = spi_flash_get_id();
    const uint8_t capacity = (id >> 16) & 0xff;
    if ((capacity < 0x14) || (capacity > 0x18)) {
        return;
    }

    const uint32_t size = uint32_t{1} << capacity;
    if (size > g_rom_flashchip.chip_size) {
        g_rom_flashchip.chip_size = size;
    }
}

} // namespace

// no NVS (sdkconfig.defaults: the Wi-Fi settings and the radio calibration are not kept there)
extern "C" void app_main() {
    flash_size_from_id();
    xTaskCreate(loop_task, "loopTask", 6144 / sizeof(StackType_t), nullptr, 1, &loopTaskHandle);
}
