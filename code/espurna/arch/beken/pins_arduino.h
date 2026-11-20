/*

The pins of the board for the libraries (OneWire, NewPing, RFM69 include pins_arduino.h or take SS & co. of Arduino.h),
the BK7231N ones: the console UART (UART1, the flashing pads), the I2C1 and the SPI pads of the chip, the one ADC pin.
ESPurna's own code takes its pins from the settings

*/

#pragma once

#include <stdint.h>

static const uint8_t TX = 11;
static const uint8_t RX = 10;

static const uint8_t SDA = 21;
static const uint8_t SCL = 20;

static const uint8_t SS = 15;
static const uint8_t MOSI = 16;
static const uint8_t MISO = 17;
static const uint8_t SCK = 14;

static const uint8_t A0 = 23;
