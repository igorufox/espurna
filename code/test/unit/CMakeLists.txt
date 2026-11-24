# unit tests of ESPurna's own code on the host compiler, without Arduino and without a chip SDK: host/ stands in for
# arch/<chip>. Fetched: https://github.com/ThrowTheSwitch/Unity and https://github.com/igorufox/espurna-base
#
# after everything is installed
# $ cmake -B build
# $ cmake --build build --target test

cmake_minimum_required(VERSION 3.18)
project(host-test VERSION 1 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11 CACHE STRING "Global C standard version (...does not yet work with 17 though)")
set(CMAKE_CXX_STANDARD 17 "Global C++ standard version")

set(COMMON_FLAGS
    -Os
    -g
    -fno-common
    -funsigned-char
    -DPROGMEM_STRING_ATTR=
    -DHOST_MOCK=1
    -DUNITY_INCLUDE_DOUBLE
)

set(ESPURNA_PATH ${CMAKE_SOURCE_DIR}/../../../ CACHE PATH "ESPurna source code repo root")

# Make sure to generate compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# PIO does not really make it easy to install packages outside of the 'platform' context,
# so sharing these between a normal builder might not be an option. (...big TODO though)
# for right now, just fetch these as raw repos. plus, there's no need for any extra params

set(unity_version v2.6.0)

include(FetchContent)
FetchContent_Declare(
    unitygit
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity
    GIT_TAG ${unity_version}
    GIT_CONFIG core.autocrlf=false core.eol=lf
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/cache/unitygit-${unity_version}-src
    SUBBUILD_DIR ${CMAKE_SOURCE_DIR}/cache/unitygit-${unity_version}-subbuild
)

FetchContent_MakeAvailable(unitygit)
FetchContent_GetProperties(unitygit SOURCE_DIR)

target_compile_options(unity BEFORE PRIVATE
    -DUNITY_OUTPUT_COLOR
    -DUNITY_INCLUDE_DOUBLE
)

# espurna-base: ESPurna's string (espurna_string.h), the same release as the firmware ([libraries] espurna_base of
# code/libraries.ini)
set(espurna_base_version 1.0.0)

FetchContent_Declare(
    espurnabase
    GIT_REPOSITORY https://github.com/igorufox/espurna-base
    GIT_TAG ${espurna_base_version}
    GIT_CONFIG core.autocrlf=false core.eol=lf
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/cache/espurna-base-${espurna_base_version}-src
    SUBBUILD_DIR ${CMAKE_SOURCE_DIR}/cache/espurna-base-${espurna_base_version}-subbuild
)

FetchContent_MakeAvailable(espurnabase)
FetchContent_GetProperties(espurnabase SOURCE_DIR)

# our library source (maybe some day this will be a simple glob)
add_library(espurna STATIC
    ${espurnabase_SOURCE_DIR}/src/espurna_string.cpp
    ${CMAKE_SOURCE_DIR}/host/host.cpp
    ${ESPURNA_PATH}/code/espurna/byte_stream.cpp
    ${ESPURNA_PATH}/code/espurna/text_types.cpp
    ${ESPURNA_PATH}/code/espurna/json.cpp
    ${ESPURNA_PATH}/code/espurna/fs_math.c
    ${ESPURNA_PATH}/code/espurna/settings_convert.cpp
    ${ESPURNA_PATH}/code/espurna/terminal_commands.cpp
    ${ESPURNA_PATH}/code/espurna/terminal_parsing.cpp
    ${ESPURNA_PATH}/code/espurna/bits.cpp
    ${ESPURNA_PATH}/code/espurna/datetime.cpp
    ${ESPURNA_PATH}/code/espurna/types.cpp
    ${ESPURNA_PATH}/code/espurna/utils.cpp
)
target_link_options(espurna PUBLIC
    -fsanitize=address
)
target_include_directories(espurna PUBLIC
    # the host in place of the chip: what the firmware takes from arch/<chip> (host/README)
    ${CMAKE_SOURCE_DIR}/host
    # the chip interface every chip implements (chip_sdk.h), host/host.cpp for the tests
    ${ESPURNA_PATH}/code/espurna/arch/rtos
    ${ESPURNA_PATH}/code/
    ${ESPURNA_PATH}/code/espurna/
    ${espurnabase_SOURCE_DIR}/src
)
# json.cpp is the code of the modules that read JSON (MODULE_BUILD)
set_source_files_properties(${ESPURNA_PATH}/code/espurna/json.cpp PROPERTIES COMPILE_DEFINITIONS MODULE_BUILD=1)

target_compile_options(espurna PUBLIC
    ${COMMON_FLAGS}
    -fsanitize=address
)
target_compile_options(espurna PRIVATE
    -Wall
    -Wextra
)

# extra code linked w/ the unity for every test executable
add_library(unity-local STATIC
    ${CMAKE_SOURCE_DIR}/unity/unity_fixtures.c
    ${CMAKE_SOURCE_DIR}/unity/unity_extra.cpp
)
target_link_libraries(unity-local PUBLIC espurna unity)
target_include_directories(unity-local PUBLIC
    ${CMAKE_SOURCE_DIR}/unity
)
target_compile_options(unity-local PUBLIC
    ${COMMON_FLAGS}
)

# each case is built separately, we expect these to work like a normal executable
include(CTest)
list(APPEND CMAKE_CTEST_ARGUMENTS "--output-on-failure")

function(build_tests)
    foreach(ARG IN LISTS ARGN)
        file(GLOB ${ARG}_sources "src/${ARG}/*.h" "src/${ARG}/*.cpp")
        add_executable(test-${ARG} ${${ARG}_sources})
        target_link_libraries(test-${ARG} espurna unity unity-local)
        target_compile_options(test-${ARG} PRIVATE
            ${COMMON_FLAGS}
            -Wall
            -Wextra
        )
        set_target_properties(test-${ARG} PROPERTIES COMPILE_FLAGS -g)
        add_test(NAME ${ARG} COMMAND test-${ARG})
    endforeach()
endfunction()

# the vsnprintf of the ESP8266 (arch/esp8266/stdio_esp8266.c) against the host's C library, every case
add_executable(test-stdio_esp8266 src/stdio_esp8266/compare.c)
target_include_directories(test-stdio_esp8266 PRIVATE src/stdio_esp8266/host)
target_compile_options(test-stdio_esp8266 PRIVATE -O1 -ffp-contract=off)
add_test(NAME stdio_esp8266 COMMAND test-stdio_esp8266)

build_tests(
    datetime
    embedis
    filters
    led
    modules
    mqtt
    pages
    scheduler
    sensor
    settings
    terminal
    tuya
    types
    url
    utils
)
