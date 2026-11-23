/*

SPI MODULE: the one software SPI master (chip::spi_* of arch/rtos/chip_sdk.h) of every chip without a hardware port
ESPurna drives (Beken). Any three pins, on espurna::pins (pins.h) and chip::delay_us() only - nothing of a chip's SDK.
ESP8266 (HSPI) and ESP32 have their hardware ports (arch/<chip>/spi_<chip>.cpp; arch/soft/ is not in ESP32's
CMakeLists.txt, the ESP8266 build leaves this file out of its spi module). In the spi module only (custom_modules of
platformio_beken.ini), the head has no SPI

*/

#include "espurna.h"

#if SPI_SUPPORT

#include <chip_sdk.h>

#include "../../pins.h"

#include <algorithm>
#include <cstdint>

namespace espurna {
namespace core {
namespace chip {

// --- SPI: a software master on any three pins: up to 500 kHz by half periods of 1 us and more, above that as fast as the
// pins go (no delay: a TFT of the lcd module) -------------------------------------------------------------------------

namespace {

struct Spi {
    int sck { -1 };
    int miso { -1 };
    int mosi { -1 };
    uint8_t mode { 0 };
    bool msb_first { true };
    uint32_t half { 1 };
};

Spi spi;

uint8_t spi_byte(uint8_t data) {
    const bool idle = (spi.mode & 0x02) != 0;   // CPOL
    const bool late = (spi.mode & 0x01) != 0;   // CPHA: data on the second edge

    uint8_t out = 0;
    for (int bit = 0; bit < 8; ++bit) {
        const int shift = spi.msb_first ? (7 - bit) : bit;
        const bool value = (data >> shift) & 1;
        if (!late && (spi.mosi >= 0)) {
            pins::write(spi.mosi, value);
        }

        if (spi.half) {
            delay_us(spi.half);
        }
        pins::write(spi.sck, idle ? 0 : 1);
        if (late && (spi.mosi >= 0)) {
            pins::write(spi.mosi, value);
        }

        if (!late && (spi.miso >= 0) && pins::read(spi.miso)) {
            out |= (1 << shift);
        }

        if (spi.half) {
            delay_us(spi.half);
        }
        pins::write(spi.sck, idle ? 1 : 0);
        if (late && (spi.miso >= 0) && pins::read(spi.miso)) {
            out |= (1 << shift);
        }
    }

    return out;
}

} // namespace

bool spi_begin(uint8_t sck, uint8_t miso, uint8_t mosi) {
    if (!pin_valid(sck)) {
        return false;
    }

    spi.sck = sck;
    spi.miso = pin_valid(miso) ? miso : -1;
    spi.mosi = pin_valid(mosi) ? mosi : -1;

    pins::mode(spi.sck, pins::Output);
    pins::write(spi.sck, (spi.mode & 0x02) ? 1 : 0);
    if (spi.mosi >= 0) {
        pins::mode(spi.mosi, pins::Output);
    }
    if (spi.miso >= 0) {
        pins::mode(spi.miso, pins::Input);
    }

    return true;
}

void spi_end() {
    spi.sck = spi.miso = spi.mosi = -1;
}

void spi_config(uint32_t frequency, uint8_t mode, bool msb_first) {
    spi.mode = mode & 0x03;
    spi.msb_first = msb_first;
    spi.half = !frequency ? 1 : (frequency > 500000) ? 0 : std::max<uint32_t>(1, 500000 / frequency);
    if (spi.sck >= 0) {
        pins::write(spi.sck, (spi.mode & 0x02) ? 1 : 0);
    }
}

void spi_transfer(const uint8_t* out, uint8_t* in, size_t size) {
    if (spi.sck < 0) {
        return;
    }

    for (size_t index = 0; index < size; ++index) {
        const uint8_t value = spi_byte(out ? out[index] : 0xff);
        if (in) {
            in[index] = value;
        }
    }
}

} // namespace chip
} // namespace core
} // namespace espurna

#endif // SPI_SUPPORT
