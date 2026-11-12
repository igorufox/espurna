#include "SPI.h"

SPIClass SPI;

void SPIClass::begin(int8_t sck, int8_t miso, int8_t mosi, int8_t) {
    _sck = sck;
    _miso = miso;
    _mosi = mosi;
    begin();
}

void SPIClass::begin() {
    if (_sck < 0) {
        return;
    }

    pinMode(_sck, OUTPUT);
    digitalWrite(_sck, (_mode & 0x02) ? HIGH : LOW);
    if (_mosi >= 0) {
        pinMode(_mosi, OUTPUT);
    }
    if (_miso >= 0) {
        pinMode(_miso, INPUT);
    }
}

void SPIClass::end() {
}

void SPIClass::beginTransaction(const SPISettings& settings) {
    setDataMode(settings.data_mode);
    setBitOrder(settings.bit_order);
    setFrequency(settings.clock);
}

void SPIClass::endTransaction() {
}

void SPIClass::setDataMode(uint8_t mode) {
    _mode = mode & 0x03;
    if (_sck >= 0) {
        digitalWrite(_sck, (_mode & 0x02) ? HIGH : LOW);
    }
}

void SPIClass::setBitOrder(uint8_t order) {
    _order = order;
}

void SPIClass::setClockDivider(uint32_t divider) {
    setFrequency(divider ? (16000000 / divider) : 1000000);
}

// a software clock: 500 kHz at most (1 us per half), slower when asked
void SPIClass::setFrequency(uint32_t frequency) {
    _half = frequency ? std::max<uint32_t>(1, 500000 / frequency) : 1;
}

uint8_t SPIClass::transfer(uint8_t data) {
    if (_sck < 0) {
        return 0;
    }

    const bool idle = (_mode & 0x02) != 0;      // CPOL
    const bool late = (_mode & 0x01) != 0;      // CPHA: data on the second edge
    uint8_t out = 0;

    for (int bit = 0; bit < 8; ++bit) {
        const int shift = (_order == MSBFIRST) ? (7 - bit) : bit;
        const bool value = (data >> shift) & 1;

        if (!late && (_mosi >= 0)) {
            digitalWrite(_mosi, value ? HIGH : LOW);
        }
        delayMicroseconds(_half);
        digitalWrite(_sck, idle ? LOW : HIGH);
        if (late && (_mosi >= 0)) {
            digitalWrite(_mosi, value ? HIGH : LOW);
        }
        if (!late && (_miso >= 0) && digitalRead(_miso)) {
            out |= (1 << shift);
        }
        delayMicroseconds(_half);
        digitalWrite(_sck, idle ? HIGH : LOW);
        if (late && (_miso >= 0) && digitalRead(_miso)) {
            out |= (1 << shift);
        }
    }

    return out;
}

uint16_t SPIClass::transfer16(uint16_t data) {
    if (_order == MSBFIRST) {
        const uint16_t high = transfer(static_cast<uint8_t>(data >> 8));
        return static_cast<uint16_t>((high << 8) | transfer(static_cast<uint8_t>(data)));
    }

    const uint16_t low = transfer(static_cast<uint8_t>(data));
    return static_cast<uint16_t>(low | (transfer(static_cast<uint8_t>(data >> 8)) << 8));
}

void SPIClass::transfer(void* buffer, size_t size) {
    auto* bytes = static_cast<uint8_t*>(buffer);
    for (size_t index = 0; index < size; ++index) {
        bytes[index] = transfer(bytes[index]);
    }
}

void SPIClass::transferBytes(const uint8_t* out, uint8_t* in, size_t size) {
    for (size_t index = 0; index < size; ++index) {
        const uint8_t value = transfer(out ? out[index] : 0xff);
        if (in) {
            in[index] = value;
        }
    }
}
