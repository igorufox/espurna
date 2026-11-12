/*

SPI MODULE

The one SPI bus of the board (the spi module, docs/modules.md): pins spiSCK / spiMISO / spiMOSI, the chip's SPI
(arch/<chip>/spi_chip.h). The modules on SPI (rfm69, mcp23s08) need it and are linked against it directly.

*/

#pragma once

#include <cstdint>

struct SpiPins {
    uint8_t sck;
    uint8_t miso;
    uint8_t mosi;
};

// the bus started (the pins set, valid and free); a device on it checks this before it talks
bool spiReady();
SpiPins spiPins();

void spiSetup();
