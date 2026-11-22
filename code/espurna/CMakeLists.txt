# The sources of ESPurna's ESP32 build (ESP-IDF; build_src_filter does not work with it): every file outside arch/ but
# the module objects (module_*.cpp, built by scripts/module_link.py), and of arch/ only the ESP32's (esp32, rtos).
# The ESP8266 and Beken builds do not read this file
file(GLOB_RECURSE app_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
list(FILTER app_sources EXCLUDE REGEX ".*/arch/.*")
file(GLOB chip_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/esp32/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/esp32/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/rtos/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/rtos/*.cpp)
list(APPEND app_sources ${chip_sources})
list(FILTER app_sources EXCLUDE REGEX ".*/module_[^/]*\\.cpp$")

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "." "arch/esp32" "arch/rtos"
    REQUIRES
        app_update
        bootloader_support
        bt
        driver
        efuse
        esp_adc
        esp_app_format
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_rmt
        esp_driver_spi
        esp_driver_uart
        esp_event
        esp_netif
        esp_partition
        esp_phy
        esp_timer
        esp_wifi
        espcoredump
        lwip
        mbedtls
        nvs_flash
        spi_flash)

# ringbuf.c's event list calls through the kernel's critical on the SMP kernel (arch/esp32/ringbuf_smp_esp32.c)
if(CONFIG_FREERTOS_SMP)
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=xTaskRemoveFromEventList"
        "-Wl,--wrap=vTaskPlaceOnEventList")
endif()
