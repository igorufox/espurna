// setup() once, then loop() for ever: the body of the loop task. The chip entry (app_main of ESP-IDF and
// ESP8266_RTOS_SDK, main() of LibreTiny base) starts the task with its stack and core and calls run()

#include <Arduino.h>

#include "core_chip.h"

namespace espurna {
namespace core {

[[noreturn]] void run() {
    setup();
    for (;;) {
        chip::loop_pass();
        loop();
    }
}

} // namespace core
} // namespace espurna
