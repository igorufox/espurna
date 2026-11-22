# ESPurna's step of the ESP32 bootloader (ESP-IDF takes the project's bootloader_components into the bootloader): the
# bootloader step of every chip, arch/rtos/bootloader/espurna_rbl.c, with the ESP32's part (arch/esp32/bootloader/
# rbl_chip.h). A new head from its RBL package, owner 22.11: the ESP8266's scheme on the ESP32
set(espurna ${CMAKE_CURRENT_LIST_DIR}/../../espurna)
idf_component_register(SRCS "${espurna}/arch/rtos/bootloader/espurna_rbl.c"
    INCLUDE_DIRS "${espurna}/arch/esp32/bootloader"
    PRIV_REQUIRES bootloader_support hal esp_rom)
