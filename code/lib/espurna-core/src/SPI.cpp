#include "SPI.h"

#include "core_chip.h"

namespace chip = espurna::core::chip;

SPIClass SPI;

void SPIClass::pins(int8_t sck, int8_t miso, int8_t mosi, int8_t) {
    _sck = sck;
    _miso = miso;
    _mosi = mosi;
}

bool SPIClass::begin(int8_t sck, int8_t miso, int8_t mosi, int8_t ss) {
    if (sck >= 0) {
        pins(sck, miso, mosi, ss);
    }

    if (_sck < 0) {
        return false;
    }

    end();

    const auto pin = [](int8_t value) {
        return (value < 0) ? chip::NoPin : static_cast<uint8_t>(value);
    };

    _started = chip::spi_begin(pin(_sck), pin(_miso), pin(_mosi));
    if (_started) {
        apply();
    }

    return _started;
}

void SPIClass::begin() {
    begin(-1, -1, -1, -1);
}

void SPIClass::end() {
    if (_started) {
        chip::spi_end();
        _started = false;
    }
}

void SPIClass::apply() {
    if (_started) {
        chip::spi_config(_frequency, _mode, _msb_first);
    }
}

void SPIClass::beginTransaction(arduino::SPISettings settings) {
    _frequency = settings.getClockFreq();
    _mode = static_cast<uint8_t>(settings.getDataMode());
    _msb_first = settings.getBitOrder() == MSBFIRST;
    apply();
}

void SPIClass::endTransaction() {
}

void SPIClass::setFrequency(uint32_t frequency) {
    _frequency = frequency;
    apply();
}

void SPIClass::setClockDivider(uint32_t divider) {
    setFrequency(BaseClock / (divider ? divider : 1));
}

void SPIClass::setDataMode(uint8_t mode) {
    _mode = mode & 3;
    apply();
}

void SPIClass::setBitOrder(uint8_t order) {
    _msb_first = order == MSBFIRST;
    apply();
}

uint8_t SPIClass::transfer(uint8_t data) {
    uint8_t out { 0 };
    chip::spi_transfer(&data, &out, 1);
    return out;
}

uint16_t SPIClass::transfer16(uint16_t data) {
    uint8_t buffer[2];
    if (_msb_first) {
        buffer[0] = data >> 8;
        buffer[1] = data & 0xff;
    } else {
        buffer[0] = data & 0xff;
        buffer[1] = data >> 8;
    }

    chip::spi_transfer(buffer, buffer, 2);

    return _msb_first
        ? static_cast<uint16_t>((buffer[0] << 8) | buffer[1])
        : static_cast<uint16_t>((buffer[1] << 8) | buffer[0]);
}

void SPIClass::transfer(void* buffer, size_t size) {
    auto* data = static_cast<uint8_t*>(buffer);
    chip::spi_transfer(data, data, size);
}

void SPIClass::transferBytes(const uint8_t* out, uint8_t* in, uint32_t size) {
    chip::spi_transfer(out, in, size);
}

void SPIClass::writeBytes(const uint8_t* data, uint32_t size) {
    chip::spi_transfer(data, nullptr, size);
}
