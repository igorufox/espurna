#include <Arduino.h>

#include "core_chip.h"

namespace chip = espurna::core::chip;

HardwareSerialPort Serial;

bool HardwareSerialPort::begin(unsigned long baud, uint16_t config, uint8_t rx, uint8_t tx,
        bool invert, size_t rx_buffer)
{
    end();
    _port = chip::uart_open(baud, config, rx, tx, invert, rx_buffer);
    return _port >= 0;
}

void HardwareSerialPort::end() {
    if (_port >= 0) {
        chip::uart_close(_port);
        _port = -1;
    }
}

void HardwareSerialPort::updateBaudRate(unsigned long baud) {
    if (_port >= 0) {
        chip::uart_baud(_port, baud);
    }
}

int HardwareSerialPort::available() {
    return (_port >= 0)
        ? static_cast<int>(chip::uart_available(_port))
        : 0;
}

int HardwareSerialPort::peek() {
    return (_port >= 0)
        ? chip::uart_peek(_port)
        : -1;
}

int HardwareSerialPort::read() {
    uint8_t out;
    return (read(&out, 1) == 1) ? out : -1;
}

size_t HardwareSerialPort::read(uint8_t* buffer, size_t size) {
    return (_port >= 0)
        ? chip::uart_read(_port, buffer, size)
        : 0;
}

int HardwareSerialPort::availableForWrite() {
    return (_port >= 0)
        ? static_cast<int>(chip::uart_write_space(_port))
        : 0;
}

void HardwareSerialPort::flush() {
    if (_port >= 0) {
        chip::uart_flush(_port);
    }
}

size_t HardwareSerialPort::write(uint8_t value) {
    return write(&value, 1);
}

size_t HardwareSerialPort::write(const uint8_t* buffer, size_t size) {
    return (_port >= 0)
        ? chip::uart_write(_port, buffer, size)
        : 0;
}
