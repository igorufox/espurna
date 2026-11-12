/*

The mcp23s08 module (docs/modules.md): the MCP23S08 GPIO expander on SPI: its pins for the relays and buttons, so it starts before them.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "mcp23s08.h"

ESPURNA_MODULE(mcp23s08, "mcp23s08", 7) {
    MCP23S08Setup();
}

#endif // MODULE_BUILD
