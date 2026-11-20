/*

The pins of the board for the libraries (OneWire, NewPing include pins_arduino.h), the generic ESP8266 ones of the
esp8266 Arduino core: the console UART, the default I2C and HSPI pins, the TOUT pad. ESPurna's own code takes its pins
from the settings

*/

#pragma once

#include <stdint.h>

static const uint8_t TX = 1;
static const uint8_t RX = 3;

static const uint8_t SDA = 4;
static const uint8_t SCL = 5;

static const uint8_t SS = 15;
static const uint8_t MOSI = 13;
static const uint8_t MISO = 12;
static const uint8_t SCK = 14;

static const uint8_t A0 = 17;
