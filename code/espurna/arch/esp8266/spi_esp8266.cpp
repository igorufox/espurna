/*

SPI MODULE, ESP8266 on the RTOS SDK: chip::spi_* of arch/rtos/chip_sdk.h on the HSPI driver of the SDK.
In the spi module only (custom_modules of platformio_esp8266.ini), the head has no SPI

*/

#include "espurna.h"

#if SPI_SUPPORT

#include <chip_sdk.h>

#include <freertos/FreeRTOS.h>

#include <driver/spi.h>

#include <string.h>

namespace espurna {
namespace core {
namespace esp8266 {
namespace {

// -----------------------------------------------------------------------------
// SPI master: HSPI, its own pins, CS by the caller; up to 64 bytes per transaction of the driver
// -----------------------------------------------------------------------------

bool spi_started { false };
uint32_t spi_frequency { 1000000 };
uint8_t spi_mode_value { 0 };
bool spi_msb_first { true };

spi_clk_div_t spi_divider(uint32_t frequency) {
    uint32_t div = frequency ? (80000000 / frequency) : 80;
    if (!div) {
        div = 1;
    }

    return static_cast<spi_clk_div_t>(div);
}

void spi_apply() {
    spi_interface_t interface{};
    interface.cpol = (spi_mode_value >> 1) & 1;
    interface.cpha = spi_mode_value & 1;
    interface.bit_tx_order = spi_msb_first ? 0 : 1;
    interface.bit_rx_order = spi_msb_first ? 0 : 1;
    interface.byte_tx_order = 0;
    interface.byte_rx_order = 0;
    interface.mosi_en = 1;
    interface.miso_en = 1;
    interface.cs_en = 0;
    spi_set_interface(HSPI_HOST, &interface);

    auto div = spi_divider(spi_frequency);
    spi_set_clk_div(HSPI_HOST, &div);
}

} // namespace
} // namespace esp8266

namespace chip {

using namespace esp8266;

// HSPI has its own pins (SCK 14, MISO 12, MOSI 13): others are not taken
bool spi_begin(uint8_t sck, uint8_t miso, uint8_t mosi) {
    spi_end();

    const auto pin = [](uint8_t value, uint8_t own) {
        return (value == NoPin) || (value == own);
    };
    if (!pin(sck, 14) || !pin(miso, 12) || !pin(mosi, 13)) {
        return false;
    }

    spi_config_t config{};
    config.interface.val = 0;
    config.interface.mosi_en = 1;
    config.interface.miso_en = 1;
    config.intr_enable.val = 0;
    config.event_cb = nullptr;
    config.mode = SPI_MASTER_MODE;
    config.clk_div = spi_divider(spi_frequency);
    if (spi_init(HSPI_HOST, &config) != ESP_OK) {
        return false;
    }

    spi_started = true;
    spi_apply();
    return true;
}

void spi_end() {
    if (spi_started) {
        spi_deinit(HSPI_HOST);
        spi_started = false;
    }
}

void spi_config(uint32_t frequency, uint8_t mode, bool msb_first) {
    if ((frequency == spi_frequency) && (mode == spi_mode_value) && (msb_first == spi_msb_first)) {
        return;
    }

    spi_frequency = frequency;
    spi_mode_value = mode;
    spi_msb_first = msb_first;
    if (spi_started) {
        spi_apply();
    }
}

void spi_transfer(const uint8_t* out, uint8_t* in, size_t size) {
    if (!spi_started || !size) {
        return;
    }

    constexpr size_t Chunk { 64 };
    uint32_t mosi[Chunk / 4];
    uint32_t miso[Chunk / 4];

    while (size) {
        const size_t part = (size < Chunk) ? size : Chunk;

        memset(mosi, 0xff, sizeof(mosi));
        if (out) {
            memcpy(mosi, out, part);
        }

        spi_trans_t trans{};
        trans.mosi = mosi;
        trans.miso = in ? miso : nullptr;
        trans.bits.mosi = static_cast<uint32_t>(part * 8);
        trans.bits.miso = in ? static_cast<uint32_t>(part * 8) : 0;
        spi_trans(HSPI_HOST, &trans);

        if (in) {
            memcpy(in, miso, part);
            in += part;
        }
        if (out) {
            out += part;
        }

        size -= part;
    }
}

} // namespace chip
} // namespace core
} // namespace espurna

#endif
