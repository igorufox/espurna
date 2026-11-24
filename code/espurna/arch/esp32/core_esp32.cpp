/*

ESP32 part of the layer without Arduino (lib/espurna-core core_chip.h): the ESP-IDF calls under the Arduino names,
the loop task and app_main(). What Arduino-ESP32 did at start (initArduino(), loopTask()) is here, the same order
and settings: NVS first (erased when broken or of a newer format), the loop task with 8 KB of stack on the last core,
priority 1, its task watchdog once enableLoopWDT() turns it on

*/

#include <wifi_sdk.h>

#include <chip_sdk.h>
#include <core_arduino_chip.h>
#include <rbl_esp32.h>

#include <driver/gpio.h>
#include <driver/uart.h>
#include <driver/rmt_tx.h>
#include <driver/rmt_encoder.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>

#include <esp_chip_info.h>
#include <esp_cpu.h>
#include <esp_event.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_image_format.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <dhcpserver/dhcpserver.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_private/esp_clk.h>
#include <esp_rom_sys.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <soc/efuse_reg.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string.h>

// the loop task (arch/rtos/tasks_chip.h) and whether its task watchdog is on (system_esp32.cpp)
TaskHandle_t loopTaskHandle { nullptr };
bool loopTaskWDTEnabled { false };

// system_esp32.cpp: the loop task watchdog, Arduino-ESP32's name
void enableLoopWDT() {
    if (!loopTaskWDTEnabled && loopTaskHandle) {
        if (esp_task_wdt_add(loopTaskHandle) == ESP_OK) {
            loopTaskWDTEnabled = true;
        }
    }
}

namespace espurna {
namespace core {
namespace esp32 {
namespace {

portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

// -----------------------------------------------------------------------------
// ADC: oneshot units, created on first use, 11 dB attenuation (Arduino's default, 0..3.1 V)
// -----------------------------------------------------------------------------

namespace adc {

adc_oneshot_unit_handle_t units[2] {};
adc_cali_handle_t calibrations[2] {};
uint64_t configured { 0 };

bool channel(uint8_t pin, adc_unit_t& unit, adc_channel_t& channel) {
    return adc_oneshot_io_to_channel(pin, &unit, &channel) == ESP_OK;
}

adc_oneshot_unit_handle_t prepare(uint8_t pin, adc_unit_t& unit, adc_channel_t& out) {
    if (!channel(pin, unit, out)) {
        return nullptr;
    }

    const auto index = static_cast<size_t>(unit);
    if (index >= 2) {
        return nullptr;
    }

    if (!units[index]) {
        adc_oneshot_unit_init_cfg_t config{};
        config.unit_id = unit;
        if (adc_oneshot_new_unit(&config, &units[index]) != ESP_OK) {
            units[index] = nullptr;
            return nullptr;
        }
    }

    const auto bit = uint64_t{1} << pin;
    if (!(configured & bit)) {
        adc_oneshot_chan_cfg_t config{};
        config.atten = ADC_ATTEN_DB_12;
        config.bitwidth = ADC_BITWIDTH_12;
        if (adc_oneshot_config_channel(units[index], out, &config) != ESP_OK) {
            return nullptr;
        }

        configured |= bit;
    }

    return units[index];
}

int raw(uint8_t pin) {
    adc_unit_t unit;
    adc_channel_t channel;
    auto* handle = prepare(pin, unit, channel);
    if (!handle) {
        return 0;
    }

    int out { 0 };
    if (adc_oneshot_read(handle, channel, &out) != ESP_OK) {
        return 0;
    }

    return out;
}

} // namespace adc

// -----------------------------------------------------------------------------
// GPIO interrupts: the ISR service of ESP-IDF, no IRAM flag (Arduino-ESP32's default)
// -----------------------------------------------------------------------------

bool isr_service { false };

bool isr_install() {
    if (!isr_service) {
        const auto err = gpio_install_isr_service(0);
        isr_service = (err == ESP_OK) || (err == ESP_ERR_INVALID_STATE);
    }

    return isr_service;
}

gpio_int_type_t interrupt_type(uint8_t mode) {
    switch (mode) {
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
// UART: the console (0) and one more (2), as uart_chip.h picks them
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

// -----------------------------------------------------------------------------
// Wi-Fi
// -----------------------------------------------------------------------------

namespace wifi {

bool inited { false };
bool started { false };
bool ipv6 { false };

esp_netif_t* sta { nullptr };
esp_netif_t* ap { nullptr };

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
    } else {
        esp_wifi_clear_ap_list();
    }

    portENTER_CRITICAL(&lock);
    free(records);
    records = list;
    count = number;
    scan = (done.status == 0) ? Scan::Done : Scan::Failed;
    portEXIT_CRITICAL(&lock);
}

// the one handler of the events (ESPurna's driver, wifi_driver_rtos.cpp), set before the first mode
chip::WifiEventHandler handler { nullptr };

void event(void*, esp_event_base_t base, int32_t id, void* data) {
    chip::WifiEventInfo info{};
    bool out { false };
    chip::WifiEvent kind { chip::WifiEvent::ScanDone };

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_SCAN_DONE:
            scan_done(*static_cast<wifi_event_sta_scan_done_t*>(data));
            kind = chip::WifiEvent::ScanDone;
            out = true;
            break;

        case WIFI_EVENT_STA_CONNECTED: {
            const auto& connected = *static_cast<wifi_event_sta_connected_t*>(data);
            memcpy(info.bssid, connected.bssid, sizeof(info.bssid));
            kind = chip::WifiEvent::StaConnected;
            out = true;

            // the link-local address on every connect once IPv6 is on (Matter)
            if (ipv6 && sta) {
                esp_netif_create_ip6_linklocal(sta);
            }
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED: {
            const auto& disconnected = *static_cast<wifi_event_sta_disconnected_t*>(data);
            memcpy(info.bssid, disconnected.bssid, sizeof(info.bssid));
            info.reason = disconnected.reason;
            kind = chip::WifiEvent::StaDisconnected;
            out = true;
            break;
        }
        }
    } else if ((base == IP_EVENT) && (id == IP_EVENT_STA_GOT_IP)) {
        kind = chip::WifiEvent::StaGotIp;
        out = true;
    }

    if (out && handler) {
        handler(kind, info);
    }
}

// esp_netif, the default event loop, both interfaces and the driver: once, on the first mode other than NULL
bool init() {
    if (inited) {
        return true;
    }

    esp_netif_init();

    const auto loop = esp_event_loop_create_default();
    if ((loop != ESP_OK) && (loop != ESP_ERR_INVALID_STATE)) {
        return false;
    }

    sta = esp_netif_create_default_wifi_sta();
    ap = esp_netif_create_default_wifi_ap();
    if (hostname[0] && sta) {
        esp_netif_set_hostname(sta, hostname);
    }

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&config) != ESP_OK) {
        return false;
    }

    // nothing in flash: the settings of ESPurna decide every start
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, event, nullptr, nullptr);

    inited = true;
    return true;
}

esp_netif_t* netif(bool access_point) {
    return access_point ? ap : sta;
}

} // namespace wifi

} // namespace
} // namespace esp32

// -----------------------------------------------------------------------------
// The ESP32 extras of ESPurna's own drivers: the calibrated ADC (adc_chip.h); the LEDC channels are in the
// light_pwm module (pwm_esp32.cpp)
// -----------------------------------------------------------------------------

namespace esp32 {

// calibrated millivolts of the pin (line fitting, the eFuse Vref), the raw scale without calibration data
uint32_t adc_millivolts(uint8_t pin) {
    adc_unit_t unit;
    adc_channel_t channel;
    auto* handle = adc::prepare(pin, unit, channel);
    if (!handle) {
        return 0;
    }

    int raw { 0 };
    if (adc_oneshot_read(handle, channel, &raw) != ESP_OK) {
        return 0;
    }

    const auto index = static_cast<size_t>(unit);
    if (!adc::calibrations[index]) {
        adc_cali_line_fitting_config_t config{};
        config.unit_id = unit;
        config.atten = ADC_ATTEN_DB_12;
        config.bitwidth = ADC_BITWIDTH_12;
        if (adc_cali_create_scheme_line_fitting(&config, &adc::calibrations[index]) != ESP_OK) {
            adc::calibrations[index] = nullptr;
        }
    }

    int out { 0 };
    if (adc::calibrations[index] && (adc_cali_raw_to_voltage(adc::calibrations[index], raw, &out) == ESP_OK)) {
        return static_cast<uint32_t>(out);
    }

    return static_cast<uint32_t>(raw) * 3100 / 4095;
}

} // namespace esp32

// -----------------------------------------------------------------------------
// The layer
// -----------------------------------------------------------------------------

namespace chip {

using namespace esp32;

uint64_t micros() {
    return static_cast<uint64_t>(esp_timer_get_time());
}

void delay(uint32_t ms) {
    vTaskDelay(ms / portTICK_PERIOD_MS);
}

void delay_us(uint32_t us) {
    esp_rom_delay_us(us);
}

void yield() {
    vPortYield();
}

void loop_pass() {
    if (loopTaskWDTEnabled) {
        esp_task_wdt_reset();
    }
}

void irq_disable() {
    portDISABLE_INTERRUPTS();
}

void irq_enable() {
    portENABLE_INTERRUPTS();
}

bool pin_valid(uint8_t pin) {
    return GPIO_IS_VALID_GPIO(pin);
}

void pin_mode(uint8_t pin, uint8_t mode) {
    if (!GPIO_IS_VALID_GPIO(pin)) {
        return;
    }

    gpio_config_t config{};
    config.pin_bit_mask = uint64_t{1} << pin;
    config.intr_type = GPIO_INTR_DISABLE;

    switch (mode) {
    case chip::pin::Input:
        config.mode = GPIO_MODE_INPUT;
        break;
    case chip::pin::InputPullup:
        config.mode = GPIO_MODE_INPUT;
        config.pull_up_en = GPIO_PULLUP_ENABLE;
        break;
    case chip::pin::InputPulldown:
        config.mode = GPIO_MODE_INPUT;
        config.pull_down_en = GPIO_PULLDOWN_ENABLE;
        break;
    case chip::pin::Output:
        // the input stays on, digitalRead() of an output reads what it drives (as on Arduino-ESP32)
        config.mode = GPIO_MODE_INPUT_OUTPUT;
        break;
    case chip::pin::OutputOpenDrain:
        config.mode = GPIO_MODE_INPUT_OUTPUT_OD;
        break;
    }

    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
        config.mode = GPIO_MODE_INPUT;
    }

    gpio_config(&config);
}

void pin_write(uint8_t pin, bool level) {
    gpio_set_level(static_cast<gpio_num_t>(pin), level ? 1 : 0);
}

bool pin_read(uint8_t pin) {
    return gpio_get_level(static_cast<gpio_num_t>(pin)) != 0;
}

bool pin_attach(uint8_t pin, PinHandler handler, void* arg, uint8_t mode) {
    if (!GPIO_IS_VALID_GPIO(pin) || !isr_install()) {
        return false;
    }

    const auto num = static_cast<gpio_num_t>(pin);
    gpio_isr_handler_remove(num);
    gpio_set_intr_type(num, interrupt_type(mode));
    if (gpio_isr_handler_add(num, handler, arg) != ESP_OK) {
        return false;
    }

    gpio_intr_enable(num);
    return true;
}

void pin_detach(uint8_t pin) {
    if (!GPIO_IS_VALID_GPIO(pin) || !isr_service) {
        return;
    }

    const auto num = static_cast<gpio_num_t>(pin);
    gpio_intr_disable(num);
    gpio_set_intr_type(num, GPIO_INTR_DISABLE);
    gpio_isr_handler_remove(num);
}

// 0..1023, the ESP8266 range analog buttons and sensors expect (Arduino-ESP32: analogReadResolution(10))
int adc_read(uint8_t pin) {
    return adc::raw(pin) >> 2;
}

uint8_t adc_bits() {
    return 10;
}

int uart_open(uint32_t baud, uint16_t config, uint8_t rx, uint8_t tx, bool invert, size_t rx_buffer) {
    const bool console = ((rx == NoPin) && (tx == NoPin))
        || (((rx == 3) || (rx == NoPin)) && ((tx == 1) || (tx == NoPin)));
    const uart_port_t port = console ? UART_NUM_0 : UART_NUM_2;

    if (uarts[port].open) {
        uart_close(port);
    }

    uart_config_t settings{};
    settings.baud_rate = static_cast<int>(baud);
    settings.data_bits = data_bits(config);
    settings.parity = parity(config);
    settings.stop_bits = stop_bits(config);
    settings.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    settings.source_clk = UART_SCLK_DEFAULT;

    // the driver wants more than its FIFO for RX
    const size_t rx_size = (rx_buffer > UART_HW_FIFO_LEN(port)) ? rx_buffer : (UART_HW_FIFO_LEN(port) * 2);
    if (uart_driver_install(port, static_cast<int>(rx_size), 256, 0, nullptr, 0) != ESP_OK) {
        return -1;
    }

    uart_param_config(port, &settings);

    const int rx_pin = console ? 3 : ((rx == NoPin) ? UART_PIN_NO_CHANGE : rx);
    const int tx_pin = console ? 1 : ((tx == NoPin) ? UART_PIN_NO_CHANGE : tx);
    uart_set_pin(port, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    if (invert) {
        uart_set_line_inverse(port, UART_SIGNAL_RXD_INV | UART_SIGNAL_TXD_INV);
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

size_t uart_write_space(int port) {
    size_t out { 0 };
    uart_get_tx_buffer_free_size(static_cast<uart_port_t>(port), &out);
    return out;
}

size_t uart_write(int port, const uint8_t* data, size_t size) {
    const auto out = uart_write_bytes(static_cast<uart_port_t>(port), data, size);
    return (out > 0) ? static_cast<size_t>(out) : 0;
}

void uart_flush(int port) {
    uart_wait_tx_done(static_cast<uart_port_t>(port), portMAX_DELAY);
}

uint32_t heap_free() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

uint32_t heap_max_alloc() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

uint32_t flash_size() {
    uint32_t out { 0 };
    esp_flash_get_size(nullptr, &out);
    return out;
}

namespace {

// the image header of the bootloader: the flash mode and speed the board boots with
bool bootloader_header(esp_image_header_t& out) {
    return esp_flash_read(nullptr, &out, CONFIG_BOOTLOADER_OFFSET_IN_FLASH, sizeof(out)) == ESP_OK;
}

} // namespace

// QIO 0, QOUT 1, DIO 2, DOUT 3 (FlashMode_t of the cores)
uint32_t flash_mode() {
    esp_image_header_t header{};
    return bootloader_header(header)
        ? static_cast<uint32_t>(header.spi_mode)
        : 0;
}

uint32_t flash_speed() {
    esp_image_header_t header{};
    if (!bootloader_header(header)) {
        return 0;
    }

    switch (header.spi_speed) {
    case ESP_IMAGE_SPI_SPEED_DIV_2:
        return 40000000;
    case ESP_IMAGE_SPI_SPEED_DIV_3:
        return 26000000;
    case ESP_IMAGE_SPI_SPEED_DIV_4:
        return 20000000;
    case ESP_IMAGE_SPI_SPEED_DIV_1:
        return 80000000;
    }

    return 0;
}

uint32_t sketch_size() {
    const auto* running = esp_ota_get_running_partition();
    if (!running) {
        return 0;
    }

    const esp_partition_pos_t position {
        .offset = running->address,
        .size = running->size,
    };

    esp_image_metadata_t data{};
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &data) != ESP_OK) {
        return 0;
    }

    return data.image_len;
}

// one app partition (esp32_4mb.csv): what a new head has between the end of this one and the RBL header sector, the same
// rule as the ESP8266 of 1 MB
uint32_t sketch_space() {
    const uint32_t end = espurna_rbl_round(ESPURNA_RBL_HEAD_AT + sketch_size());
    return (ESPURNA_RBL_HEADER_AT > end) ? (ESPURNA_RBL_HEADER_AT - end) : 0;
}

uint32_t cycle_count() {
    return esp_cpu_get_cycle_count();
}

uint32_t cpu_mhz() {
    return static_cast<uint32_t>(esp_clk_cpu_freq() / 1000000);
}

const char* sdk_version() {
    return esp_get_idf_version();
}

// the major revision (Arduino-ESP32's getChipRevision()): ESP-IDF 5 has major * 100 + minor in esp_chip_info_t
// (22.11 DevKit: 100 for v1.0 was taken for a v3 chip, "ESP32-D0WDQ6-V3 rev100"; 301 does not fit the byte either)
uint8_t chip_revision() {
    esp_chip_info_t info{};
    esp_chip_info(&info);
    return static_cast<uint8_t>(info.revision / 100);
}

// the package of the chip, the names of Arduino-ESP32's getChipModel()
const char* chip_model() {
    const uint32_t package = REG_GET_FIELD(EFUSE_BLK0_RDATA3_REG, EFUSE_RD_CHIP_PACKAGE) & 0x7;
    const bool v3 = chip_revision() >= 3;

    switch (package) {
    case EFUSE_RD_CHIP_VER_PKG_ESP32D0WDQ6:
        return v3 ? "ESP32-D0WDQ6-V3" : "ESP32-D0WDQ6";
    case EFUSE_RD_CHIP_VER_PKG_ESP32D0WDQ5:
        return v3 ? "ESP32-D0WD-V3" : "ESP32-D0WD";
    case EFUSE_RD_CHIP_VER_PKG_ESP32D2WDQ5:
        return "ESP32-D2WD";
    case EFUSE_RD_CHIP_VER_PKG_ESP32PICOD2:
        return "ESP32-PICO-D2";
    case EFUSE_RD_CHIP_VER_PKG_ESP32PICOD4:
        return "ESP32-PICO-D4";
    case EFUSE_RD_CHIP_VER_PKG_ESP32PICOV302:
        return "ESP32-PICO-V3-02";
    case EFUSE_RD_CHIP_VER_PKG_ESP32D0WDR2V3:
        return "ESP32-D0WDR2-V3";
    }

    return "Unknown";
}

void restart() {
    esp_restart();
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

// WPA2 at least when there is a passphrase, PMF capable. The SDK picks the AP (owner 11-18, no roaming of ESPurna's
// own, wifi.cpp): every channel is scanned and the strongest AP of the SSID taken (the fast scan takes the first one it
// finds, the order by signal does nothing then). No 802.11k/v (owner 11-18: the APs do not move)
bool wifi_sta_begin(const char* ssid, const char* passphrase, uint8_t channel, const uint8_t* bssid) {
    wifi_config_t config{};
    strncpy(reinterpret_cast<char*>(config.sta.ssid), ssid, sizeof(config.sta.ssid));
    if (passphrase) {
        strncpy(reinterpret_cast<char*>(config.sta.password), passphrase, sizeof(config.sta.password));
    }

    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.threshold.authmode = (passphrase && *passphrase) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.channel = channel;
    if (bssid) {
        config.sta.bssid_set = true;
        memcpy(config.sta.bssid, bssid, sizeof(config.sta.bssid));
    }

    esp_wifi_disconnect();
    if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK) {
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
    auto* netif = wifi::sta;
    if (!netif) {
        return false;
    }

    esp_netif_dhcpc_stop(netif);

    esp_netif_ip_info_t info{};
    info.ip.addr = ip;
    info.gw.addr = gateway;
    info.netmask.addr = netmask;
    esp_netif_set_ip_info(netif, &info);

    if (!ip) {
        return esp_netif_dhcpc_start(netif) == ESP_OK;
    }

    const uint32_t servers[] {dns1, dns2};
    const esp_netif_dns_type_t types[] {ESP_NETIF_DNS_MAIN, ESP_NETIF_DNS_BACKUP};
    for (size_t index = 0; index < 2; ++index) {
        if (servers[index]) {
            esp_netif_dns_info_t dns{};
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            dns.ip.u_addr.ip4.addr = servers[index];
            esp_netif_set_dns_info(netif, types[index], &dns);
        }
    }

    return true;
}

bool wifi_sta_address(uint32_t& ip, uint32_t& netmask, uint32_t& gateway) {
    esp_netif_ip_info_t info{};
    if (!wifi::sta || (esp_netif_get_ip_info(wifi::sta, &info) != ESP_OK)) {
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
    out.authmode = record.authmode;
    return true;
}

bool wifi_hostname(const char* hostname) {
    strncpy(wifi::hostname, hostname ? hostname : "", sizeof(wifi::hostname) - 1);
    if (wifi::sta) {
        return esp_netif_set_hostname(wifi::sta, wifi::hostname) == ESP_OK;
    }

    return true;
}

const char* wifi_get_hostname() {
    return wifi::hostname;
}

bool wifi_mac(bool ap, uint8_t* out) {
    if (wifi::inited) {
        return esp_wifi_get_mac(ap ? WIFI_IF_AP : WIFI_IF_STA, out) == ESP_OK;
    }

    return esp_read_mac(out, ap ? ESP_MAC_WIFI_SOFTAP : ESP_MAC_WIFI_STA) == ESP_OK;
}

bool wifi_set_mac(bool ap, const uint8_t* mac) {
    if (!wifi::inited) {
        return false;
    }

    return esp_wifi_set_mac(ap ? WIFI_IF_AP : WIFI_IF_STA, mac) == ESP_OK;
}

uint8_t wifi_channel() {
    uint8_t primary { 0 };
    wifi_second_chan_t second;
    if (!wifi::started || (esp_wifi_get_channel(&primary, &second) != ESP_OK)) {
        return 0;
    }

    return primary;
}

void wifi_on_event(WifiEventHandler handler) {
    wifi::handler = handler;
}

bool wifi_ipv6(bool enable) {
    wifi::ipv6 = enable;
    return true;
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
    case WIFI_REASON_BSS_TRANSITION_DISASSOC: return "BSS_TRANSITION_DISASSOC";
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
    case WIFI_REASON_ROAMING: return "ROAMING";
    case WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG: return "ASSOC_COMEBACK_TIME_TOO_LONG";
    case WIFI_REASON_SA_QUERY_TIMEOUT: return "SA_QUERY_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: return "NO_AP_FOUND_W_COMPATIBLE_SECURITY";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD: return "NO_AP_FOUND_IN_RSSI_THRESHOLD";
    }

    return "UNKNOWN";
}

// Arduino-ESP32's scanNetworks(): active scan with a 100 ms minimum per channel, passive with the maximum
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

    portENTER_CRITICAL(&lock);
    wifi::scan = wifi::Scan::Running;
    portEXIT_CRITICAL(&lock);

    if (esp_wifi_scan_start(&config, false) != ESP_OK) {
        portENTER_CRITICAL(&lock);
        wifi::scan = wifi::Scan::Failed;
        portEXIT_CRITICAL(&lock);
        return false;
    }

    return true;
}

int16_t wifi_scan_count() {
    portENTER_CRITICAL(&lock);
    const auto scan = wifi::scan;
    const auto count = wifi::count;
    portEXIT_CRITICAL(&lock);

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
    portENTER_CRITICAL(&lock);
    const bool valid = (wifi::scan == wifi::Scan::Done) && wifi::records && (index < wifi::count);
    wifi_ap_record_t record{};
    if (valid) {
        record = wifi::records[index];
    }
    portEXIT_CRITICAL(&lock);

    if (!valid) {
        return false;
    }

    wifi::copy_ssid(out.ssid, record.ssid, sizeof(record.ssid));
    memcpy(out.bssid, record.bssid, 6);
    out.channel = record.primary;
    out.rssi = record.rssi;
    out.authmode = record.authmode;
    return true;
}

void wifi_scan_delete() {
    portENTER_CRITICAL(&lock);
    auto* records = wifi::records;
    wifi::records = nullptr;
    wifi::count = 0;
    if (wifi::scan != wifi::Scan::Running) {
        wifi::scan = wifi::Scan::Idle;
    }
    portEXIT_CRITICAL(&lock);

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

    return esp_wifi_set_config(WIFI_IF_AP, &config) == ESP_OK;
}

// Arduino-ESP32's softAPConfig(): the address, DHCP leases after it, the AP itself offered as the DNS server (the
// captive portal answers there)
bool wifi_ap_config(uint32_t ip, uint32_t gateway, uint32_t netmask) {
    if (!wifi::init()) {
        return false;
    }

    auto* netif = wifi::ap;
    if (!netif) {
        return false;
    }

    esp_netif_dhcps_stop(netif);

    esp_netif_ip_info_t info{};
    info.ip.addr = ip;
    info.gw.addr = gateway;
    info.netmask.addr = netmask;
    if (esp_netif_set_ip_info(netif, &info) != ESP_OK) {
        return false;
    }

    esp_netif_dns_info_t dns{};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip;
    esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);

    dhcps_offer_t offer = OFFER_DNS;
    esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));

    return esp_netif_dhcps_start(netif) == ESP_OK;
}

// dropping the AP bit of the mode is what stops it (WiFiClass::softAPdisconnect(true))
bool wifi_ap_stop() {
    return true;
}

uint32_t wifi_ap_ip() {
    esp_netif_ip_info_t info{};
    if (!wifi::ap || (esp_netif_get_ip_info(wifi::ap, &info) != ESP_OK)) {
        return 0;
    }

    return info.ip.addr;
}

bool wifi_ap_ssid(uint8_t (&out)[33]) {
    wifi_config_t config{};
    if (!wifi::started || (esp_wifi_get_config(WIFI_IF_AP, &config) != ESP_OK)) {
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

} // namespace chip
} // namespace core
} // namespace espurna

// -----------------------------------------------------------------------------
// Arduino-ESP32 3.x rmt* of the libraries (core_arduino_chip.h)
// -----------------------------------------------------------------------------

namespace {

struct RmtPin {
    int pin;
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t encoder;
};

constexpr size_t RmtPins { 8 };
RmtPin rmt_pins[RmtPins] {};

RmtPin* rmt_find(int pin) {
    for (auto& entry : rmt_pins) {
        if (entry.channel && (entry.pin == pin)) {
            return &entry;
        }
    }

    return nullptr;
}

} // namespace

extern "C" {

bool rmtInit(int pin, rmt_ch_dir_t direction, rmt_reserve_memsize_t memsize, uint32_t frequency) {
    if ((direction != RMT_TX_MODE) || rmt_find(pin)) {
        return direction == RMT_TX_MODE;
    }

    RmtPin* slot { nullptr };
    for (auto& entry : rmt_pins) {
        if (!entry.channel) {
            slot = &entry;
            break;
        }
    }

    if (!slot) {
        return false;
    }

    rmt_tx_channel_config_t config{};
    config.gpio_num = static_cast<gpio_num_t>(pin);
    config.clk_src = RMT_CLK_SRC_DEFAULT;
    config.resolution_hz = frequency;
    config.mem_block_symbols = static_cast<size_t>(memsize) * SOC_RMT_MEM_WORDS_PER_CHANNEL;
    config.trans_queue_depth = 1;
    if (rmt_new_tx_channel(&config, &slot->channel) != ESP_OK) {
        slot->channel = nullptr;
        return false;
    }

    rmt_copy_encoder_config_t encoder{};
    if (rmt_new_copy_encoder(&encoder, &slot->encoder) != ESP_OK) {
        rmt_del_channel(slot->channel);
        slot->channel = nullptr;
        return false;
    }

    rmt_enable(slot->channel);
    slot->pin = pin;
    return true;
}

bool rmtWrite(int pin, rmt_data_t* data, size_t symbols, uint32_t timeout_ms) {
    auto* entry = rmt_find(pin);
    if (!entry) {
        return false;
    }

    rmt_transmit_config_t config{};
    if (rmt_transmit(entry->channel, entry->encoder, data, symbols * sizeof(rmt_data_t), &config) != ESP_OK) {
        return false;
    }

    const int timeout = (timeout_ms == RMT_WAIT_FOR_EVER) ? -1 : static_cast<int>(timeout_ms);
    return rmt_tx_wait_all_done(entry->channel, timeout) == ESP_OK;
}

bool rmtDeinit(int pin) {
    auto* entry = rmt_find(pin);
    if (!entry) {
        return false;
    }

    rmt_disable(entry->channel);
    rmt_del_channel(entry->channel);
    rmt_del_encoder(entry->encoder);
    *entry = RmtPin{};
    return true;
}

} // extern "C"

// -----------------------------------------------------------------------------
// Start
// -----------------------------------------------------------------------------

namespace {

void nvs_init() {
    auto err = nvs_flash_init();
    if ((err == ESP_ERR_NVS_NO_FREE_PAGES) || (err == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        const auto* partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, nullptr);
        if (partition && (esp_partition_erase_range(partition, 0, partition->size) == ESP_OK)) {
            nvs_flash_init();
        }
    }
}

void loop_task(void*) {
    espurna::core::run();
}

} // namespace

#if CONFIG_BT_ENABLED
// arch/esp32/ble_ram_esp32.cpp: the RAM of the BT controller in ROM, to the heap unless the ble module is on
void bleRamSetup();
#endif

extern "C" void app_main() {
#if CONFIG_BT_ENABLED
    bleRamSetup();
#endif
    nvs_init();
    // the last core: core 1 of the two, core 0 when FreeRTOS runs on one (CONFIG_FREERTOS_UNICORE)
    xTaskCreatePinnedToCore(loop_task, "loopTask", 8192, nullptr, 1, &loopTaskHandle, portNUM_PROCESSORS - 1);
}
