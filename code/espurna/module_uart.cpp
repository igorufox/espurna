/*

The UART module (docs/modules.md), every chip alike: the hardware and software serial ports of the uartTx<n> /
uartRx<n> / uartBaud<n> settings. The head has only forwarders (modules_api.cpp, UART_MODULE): the serial log, the
relays of a UART board (Sonoff Dual, STM) and the modules that need a port (nspanel, the UART sensors, tuya ...) take
it once this module started. Starts before the modules that use it.

*/

#include "espurna.h"

#if MODULE_BUILD && UART_SUPPORT

#include "modules.h"
#include "uart.h"

#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_uart.ipp"
#endif

ESPURNA_MODULE(uart, "uart", 5) {
#if WEB_SUPPORT
    modulesUi("uart", ui_uart_data, sizeof(ui_uart_data));
#endif

    using espurna::modules::Api;

    uartSetup();

    // the real ones (renamed in this module), before anything else calls them
    modulesProvide(Api::UartPort, &uartPort);
    modulesProvide(Api::UartClaim, &uartClaim);
    modulesProvide(Api::UartClaimed, &uartClaimed);
    modulesProvide(Api::UartOnClaim, &uartOnClaim);
    modulesProvide(Api::UartOpen, &uartOpen);
    modulesProvide(Api::UartBaud, &uartBaud);
}

#endif
