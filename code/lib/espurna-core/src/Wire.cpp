#include "Wire.h"

#include "core_chip.h"

namespace chip = espurna::core::chip;

TwoWire Wire;
TwoWire& Wire1 = Wire;

void TwoWire::pins(int sda, int scl) {
    _sda = sda;
    _scl = scl;
}

bool TwoWire::begin(int sda, int scl, uint32_t frequency) {
    if (sda >= 0) {
        _sda = sda;
    }

    if (scl >= 0) {
        _scl = scl;
    }

    if (frequency) {
        _frequency = frequency;
    }

    if ((_sda < 0) || (_scl < 0)) {
        return false;
    }

    end();
    _started = chip::i2c_begin(
        static_cast<uint8_t>(_sda), static_cast<uint8_t>(_scl), _frequency);

    return _started;
}

void TwoWire::begin() {
    begin(-1, -1, 0);
}

void TwoWire::end() {
    if (_started) {
        chip::i2c_end();
        _started = false;
    }
}

void TwoWire::setClock(uint32_t frequency) {
    _frequency = frequency;
    if (_started) {
        chip::i2c_clock(frequency);
    }
}

void TwoWire::beginTransmission(uint8_t address) {
    _address = address;
    _transmitting = true;
    _tx_size = 0;
}

uint8_t TwoWire::endTransmission(bool stop) {
    if (!_transmitting) {
        return 4;
    }

    _transmitting = false;
    if (!_started) {
        return 4;
    }

    return chip::i2c_write(_address, _tx, _tx_size, stop);
}

size_t TwoWire::requestFrom(uint8_t address, size_t size, bool stop) {
    _rx_size = 0;
    _rx_index = 0;

    if (!_started) {
        return 0;
    }

    if (size > BufferSize) {
        size = BufferSize;
    }

    _rx_size = chip::i2c_read(address, _rx, size, stop);
    return _rx_size;
}

size_t TwoWire::write(uint8_t value) {
    return write(&value, 1);
}

size_t TwoWire::write(const uint8_t* data, size_t size) {
    if (!_transmitting) {
        return 0;
    }

    const size_t room = BufferSize - _tx_size;
    const size_t out = (size < room) ? size : room;
    for (size_t index = 0; index < out; ++index) {
        _tx[_tx_size++] = data[index];
    }

    return out;
}

int TwoWire::available() {
    return static_cast<int>(_rx_size - _rx_index);
}

int TwoWire::read() {
    return (_rx_index < _rx_size)
        ? _rx[_rx_index++]
        : -1;
}

int TwoWire::peek() {
    return (_rx_index < _rx_size)
        ? _rx[_rx_index]
        : -1;
}

void TwoWire::flush() {
    _rx_size = 0;
    _rx_index = 0;
    _tx_size = 0;
}
