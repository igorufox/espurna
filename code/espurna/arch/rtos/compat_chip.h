/*

esp8266 Core and SDK API shims of the FreeRTOS chips (ESP32, BK72xx with LibreTiny)

Common part. SDK headers (FreeRTOS included), chip facts and what only one of the chips needs are in
arch/<chip>/compat_sdk.h (the chip's folder, -I of its ini)

*/

#pragma once

// the owning text of ESPurna (text_types.h), declared here: compat.h comes before types.h
namespace espurna {
class string;
} // namespace espurna



#include <memory>
#include <type_traits>

#include "compat_sdk.h"
#include "chip_sdk.h"

// ESP8266-specific memory functions
#ifndef memmove_P
#define memmove_P memmove
#endif
#ifndef memcpy_P
#define memcpy_P memcpy
#endif
#ifndef strncpy_P
#define strncpy_P strncpy
#endif
#ifndef strncasecmp_P
#define strncasecmp_P strncasecmp
#endif
#ifndef strcasecmp_P
#define strcasecmp_P strcasecmp
#endif
#ifndef strcmp_P
#define strcmp_P strcmp
#endif

// PROGMEM_STRING / STRING_VIEW (ref. types.h): .rodata is mapped from flash, string literals are already there
#define PROGMEM_STRING_ATTR
#define PROGMEM_STRING(NAME, X) static constexpr char NAME[] = (X)
#define STRING_VIEW(X) ::espurna::StringView(X)
#define STRING_VIEW_INLINE(NAME, X)\
        static constexpr char __pstr__ ## NAME ##  __ [] = (X);\
        static constexpr auto NAME = ::espurna::StringView(__pstr__ ## NAME ## __)

// Missing ADC/UART/TZ constants/functions
#define ADC_VCC 255
#define ADC_TOUT 254
#undef ADC_MODE_VALUE
#define ADC_MODE_VALUE ADC_TOUT
#define TZ_Etc_UTC "UTC0"

// Stack info, uxTaskGetStackHighWaterMark returns words; convert to bytes for ESP8266 parity.
#define getFreeStack() (uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t))

// esp8266 user_interface.h names used by the common code
#ifndef REASON_DEFAULT_RST
#define REASON_DEFAULT_RST 0
#define REASON_WDT_RST 1
#define REASON_EXCEPTION_RST 2
#define REASON_SOFT_WDT_RST 3
#define REASON_SOFT_RESTART 4
#define REASON_DEEP_SLEEP_AWAKE 5
#define REASON_EXT_SYS_RST 6
#endif

// Same layout as esp8266, crash.cpp stores it (nothing fills it on these chips)
struct rst_info {
    uint32_t reason;
    uint32_t exccause;
    uint32_t epc1;
    uint32_t epc2;
    uint32_t epc3;
    uint32_t excvaddr;
    uint32_t depc;
};

#define wifi_station_get_rssi() wifiStaRssi()

// Sleep types, same names and values as the esp8266 SDK
typedef enum : int {
    NONE_SLEEP_T = 0,
    LIGHT_SLEEP_T,
    MODEM_SLEEP_T
} sleep_type_t;

// ESP.* calls of the common code (the esp8266 API names), on the SDK calls of the chip (chip_sdk.h); what differs per
// chip is in arch/<chip>/compat_<chip>.cpp. The macro below makes `ESP` this object
extern uint16_t systemVcc();
struct EspCompat {
    // SDK reason names, plus what ESPurna knows better about the last reset
    espurna::string getResetReason();
    espurna::string getResetInfo();

    // Same as esp8266, last 3 bytes of the STA MAC
    uint32_t getChipId();

    uint32_t getFlashChipSpeed();
    uint32_t getFlashChipId();

    uint32_t getFreeSketchSpace();
    uint32_t getSketchSize();

    uint32_t getMaxAllocHeap();

    bool flashRead(uint32_t addr, uint32_t* data, size_t size);

    uint16_t getVcc() { return systemVcc(); }

    uint32_t getFreeHeap() { return ::espurna::core::chip::heap_free(); }

    uint32_t magicFlashChipSize(uint8_t) { return getFlashChipSize(); }
    uint32_t getFlashChipRealSize() { return getFlashChipSize(); }
    uint32_t getFlashChipSize() { return ::espurna::core::chip::flash_size(); }
    uint32_t getFlashChipMode() { return ::espurna::core::chip::flash_mode(); }

    const char* getChipModel() { return ::espurna::core::chip::chip_model(); }
    uint8_t getChipRevision() { return ::espurna::core::chip::chip_revision(); }

    uint32_t getFreeContStack() { return uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t); }

    [[noreturn]] void restart() { ::espurna::core::chip::restart(); }
    bool eraseConfig() { return false; }

    void getHeapStats(uint32_t* free, void* max, uint8_t* frag) {
        if (free) *free = getFreeHeap();
        if (max) *(uint32_t*)max = getMaxAllocHeap();
        if (frag) *frag = 0;
    }

    uint32_t getCycleCount() { return ::espurna::core::chip::cycle_count(); }
};

extern EspCompat ESP_COMPAT;

#ifndef FLASH_SECTOR_SIZE
#define FLASH_SECTOR_SIZE 4096
#endif

#ifndef SERIAL_RX_ONLY
#define SERIAL_RX_ONLY 1
#endif
#ifndef SERIAL_TX_ONLY
#define SERIAL_TX_ONLY 2
#endif
#ifndef SERIAL_FULL
#define SERIAL_FULL 3
#endif

// std library shims for C++11
namespace std {
    #if __cplusplus < 201402L
    template<bool B, class T = void>
    using enable_if_t = typename enable_if<B, T>::type;
    #endif
    #if __cplusplus < 201703L
    template<typename F, typename... Args>
    struct invoke_result_helper {
        using type = decltype(std::declval<F>()(std::declval<Args>()...));
    };

    template<typename F, typename... Args>
    using invoke_result_t = typename invoke_result_helper<F, Args...>::type;
    #endif
}

// Special hack for settings_embedis.h
#if __cplusplus < 201703L
#undef __cpp_lib_result_of_sfinae
#endif
