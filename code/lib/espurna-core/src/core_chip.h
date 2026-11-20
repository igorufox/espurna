#pragma once
// What a chip gives the layer: ESPurna's own SDK calls of the chip (arch/rtos/chip_sdk.h, implemented in
// espurna/arch/<chip>/core_<chip>.cpp). The layer puts the Arduino names on them (wiring.cpp, Serial, Wire, SPI, ESP,
// Update, WiFi): the pin modes and interrupt edges of ArduinoCore-API have the same values as chip_sdk.h's

#include <chip_sdk.h>
