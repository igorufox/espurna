/*

SPI MODULE

The one SPI bus of the board: the pins from the settings (the chip's default where it has one), locked for the bus,
then the chip's SPI starts (arch/<chip>/spi_chip.h: ESP8266 HSPI, ESP32 any pins, Beken the software SPI).

*/

#include "espurna.h"

#if SPI_SUPPORT

#include "spi.h"
#include "gpio.h"

#include <spi_chip.h>
#include <chip_sdk.h>

namespace espurna {
namespace spi {
namespace {

SpiPins pins { GPIO_NONE, GPIO_NONE, GPIO_NONE };
bool ready { false };

uint8_t pin(const char* key, uint8_t fallback) {
    return getSetting(key, fallback);
}

} // namespace
} // namespace spi
} // namespace espurna

bool spiReady() {
    return espurna::spi::ready;
}

SpiPins spiPins() {
    return espurna::spi::pins;
}

// the bus for SPI of the Arduino emulation (chip_sdk.h SpiBus): the RFM69 library and mcp23s08 talk through it
static constexpr espurna::core::chip::SpiBus SpiBusTable {
    &espurna::core::chip::spi_begin,
    &espurna::core::chip::spi_end,
    &espurna::core::chip::spi_config,
    &espurna::core::chip::spi_transfer,
};

void spiSetup() {
    espurna::core::chip::spi_bus = &SpiBusTable;

    using namespace espurna::driver::spi;

    const SpiPins pins {
        espurna::spi::pin("spiSCK", DefaultSck),
        espurna::spi::pin("spiMISO", DefaultMiso),
        espurna::spi::pin("spiMOSI", DefaultMosi),
    };
    espurna::spi::pins = pins;

    if (!gpioValid(pins.sck) || !gpioValid(pins.miso) || !gpioValid(pins.mosi)) {
        return;
    }

    for (const auto pin : {pins.sck, pins.miso, pins.mosi}) {
        if (!gpioLock(pin)) {
            DEBUG_MSG_P(PSTR("[SPI] GPIO%hhu is already used\n"), pin);
            return;
        }
    }

    if (!begin(pins.sck, pins.miso, pins.mosi)) {
        DEBUG_MSG_P(PSTR("[SPI] The chip cannot have SCK GPIO%hhu MISO GPIO%hhu MOSI GPIO%hhu\n"),
            pins.sck, pins.miso, pins.mosi);
        return;
    }

    espurna::spi::ready = true;
    DEBUG_MSG_P(PSTR("[SPI] SCK GPIO%hhu MISO GPIO%hhu MOSI GPIO%hhu\n"), pins.sck, pins.miso, pins.mosi);
}

#endif // SPI_SUPPORT
