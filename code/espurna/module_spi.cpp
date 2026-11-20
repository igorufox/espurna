/*

The spi module (docs/modules.md): the SPI bus of the board, spi.cpp and the chip's SPI (settings spiSCK / spiMISO /
spiMOSI; the ESP8266's HSPI has its pins fixed, the BK7231N the software SPI of lib_beken/SPI). The modules on SPI
(rfm69, mcp23s08) need it and are linked against it directly.

The SENSORS panel shows its pins with the other buses, in the spec format of the drivers: `snsBus`.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "spi.h"
#include "ws.h"

namespace {

#if WEB_SUPPORT
alignas(4) static constexpr char Spec[] PROGMEM = "spiSCK|gpio|SCK|;spiMISO|gpio|MISO|;spiMOSI|gpio|MOSI|";

void onConnected(espurna::json::Writer& root) {
    const auto pins = spiPins();

    root.array(STRING_VIEW("snsBus"));
    root.beginObject();
    root.field(STRING_VIEW("name"), STRING_VIEW("spi"));
    root.field(STRING_VIEW("spec"), Spec);

    root.object(STRING_VIEW("values"));
    root.field(STRING_VIEW("spiSCK"), pins.sck);
    root.field(STRING_VIEW("spiMISO"), pins.miso);
    root.field(STRING_VIEW("spiMOSI"), pins.mosi);
    root.end();
    root.end();
    root.end();
}

bool onKeyCheck(espurna::StringView key, espurna::json::Value) {
    return key.startsWith(STRING_VIEW("spi"));
}
#endif

} // namespace

ESPURNA_MODULE(spi, "spi", 6) {
    spiSetup();
#if WEB_SUPPORT
    wsRegister()
        .onConnected(onConnected)
        .onKeyCheck(onKeyCheck);
#endif
}

#endif // MODULE_BUILD
