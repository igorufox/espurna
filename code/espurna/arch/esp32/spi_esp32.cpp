/*

The SPI master of the ESP32 (chip_sdk.h spi_*): the spi module's, not the head's (config/head.h ESPURNA_MODULES, the
custom_modules line of platformio_esp32.ini), as on every chip. VSPI (SPI3), any pins through the GPIO matrix.

*/

#include "espurna.h"

#if SPI_SUPPORT

#include <chip_sdk.h>

#include <freertos/FreeRTOS.h>
#include <driver/spi_master.h>

#include <cstring>

namespace espurna {
namespace core {
namespace chip {
namespace {

// -----------------------------------------------------------------------------
// SPI master: VSPI (SPI3), any pins through the GPIO matrix, CS by the caller
// -----------------------------------------------------------------------------

constexpr spi_host_device_t SpiHost { SPI3_HOST };

bool spi_started { false };
spi_device_handle_t spi_device { nullptr };
uint32_t spi_frequency { 1000000 };
uint8_t spi_mode { 0 };
bool spi_msb_first { true };

void spi_attach() {
    if (spi_device) {
        spi_bus_remove_device(spi_device);
        spi_device = nullptr;
    }

    spi_device_interface_config_t config{};
    config.mode = spi_mode;
    config.clock_speed_hz = static_cast<int>(spi_frequency);
    config.spics_io_num = -1;
    config.queue_size = 1;
    config.flags = spi_msb_first ? 0 : (SPI_DEVICE_TXBIT_LSBFIRST | SPI_DEVICE_RXBIT_LSBFIRST);
    spi_bus_add_device(SpiHost, &config, &spi_device);
}

} // namespace

bool spi_begin(uint8_t sck, uint8_t miso, uint8_t mosi) {
    spi_end();

    spi_bus_config_t config{};
    config.sclk_io_num = (sck == NoPin) ? -1 : sck;
    config.miso_io_num = (miso == NoPin) ? -1 : miso;
    config.mosi_io_num = (mosi == NoPin) ? -1 : mosi;
    config.quadwp_io_num = -1;
    config.quadhd_io_num = -1;
    config.max_transfer_sz = 4096;
    if (spi_bus_initialize(SpiHost, &config, SPI_DMA_CH_AUTO) != ESP_OK) {
        return false;
    }

    spi_started = true;
    spi_attach();
    return spi_device != nullptr;
}

void spi_end() {
    if (spi_device) {
        spi_bus_remove_device(spi_device);
        spi_device = nullptr;
    }

    if (spi_started) {
        spi_bus_free(SpiHost);
        spi_started = false;
    }
}

void spi_config(uint32_t frequency, uint8_t mode, bool msb_first) {
    if ((frequency == spi_frequency) && (mode == spi_mode) && (msb_first == spi_msb_first) && spi_device) {
        return;
    }

    spi_frequency = frequency;
    spi_mode = mode;
    spi_msb_first = msb_first;
    if (spi_started) {
        spi_attach();
    }
}

void spi_transfer(const uint8_t* out, uint8_t* in, size_t size) {
    if (!spi_device || !size) {
        return;
    }

    spi_transaction_t transaction{};
    transaction.length = size * 8;
    transaction.tx_buffer = out;
    transaction.rx_buffer = in;
    spi_device_polling_transmit(spi_device, &transaction);
}

} // namespace chip
} // namespace core
} // namespace espurna

#endif // SPI_SUPPORT
