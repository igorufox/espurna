# ESP-IDF project of ESPurna's ESP32 build (platformio_esp32.ini, framework = espidf). The ESP8266 and Beken builds
# (platformio_esp8266.ini, platformio_beken.ini) do not read it. The sources: espurna/CMakeLists.txt
cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espurna)
