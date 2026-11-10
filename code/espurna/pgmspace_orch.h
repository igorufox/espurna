#pragma once

// every core has its own pgmspace.h (on ESP32 and LibreTiny, PROGMEM is a no-op)
#if __has_include(<pgmspace.h>)
    #include <pgmspace.h>
#else
    #include <sys/pgmspace.h>
#endif
