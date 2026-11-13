/*

The http module (docs/modules.md): the HTTP client of the modules (http.cpp). Nothing to start: the modules that need
it make their requests. Starts before them.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"

ESPURNA_MODULE(http, "http", 4) {
}

#endif // MODULE_BUILD
