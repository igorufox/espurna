/*

CHIP DESCRIPTION AND LWIP GLUE

What the common code shows about the chip, and the few lwIP calls whose types differ between
the chips (IPv4-only lwip2 on esp8266, dual stack on ESP32 and LibreTiny).
Per chip, ref. arch/<chip>/chip_<chip>.cpp

*/

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <lwip/dns.h>
#include <lwip/ip_addr.h>

namespace espurna {
namespace chip {

// WebUI: 'arch' and 'mcu', the device template search picks the database and the pin layout by these.
// Plus whatever else the WebUI shows about the chip (e.g. Beken bootloader layout)
void ws_connected(JsonObject& root);
void ws_visible(JsonObject& root);

// Terminal INFO, the 'mcu:' line and whatever else describes the chip (e.g. Beken bootloader layout)
void info(Print& out);

// Terminal HEAP, memory besides the heap (e.g. lwIP heap and TCM of the BK7231N)
void memory(Print& out);

// Terminal NETSTAT, false when lwIP pcb lists can't be walked from loop() safely
bool netstat(Print& out);

// Terminal STORAGE of a flash split into partitions (ESP32): the chip prints all of it and returns true.
// false when the common fixed layout (app, OTA, settings, SDK) describes the flash. `mode` is the flash mode name
bool storage(Print& out, const char* mode);

// The release image this board takes (WebUI release list, its files): ESPURNA_IMAGE ("" - local build). Beken: the
// image of the bootloader the board has, "beken-tuya" or "beken-quectel"
String image();

// Framework release and its revision (INFO, build info of the WebUI / MQTT / API)
String core_version();
String core_revision();

// IPv4 part of the lwIP address
IPAddress address(const ip_addr_t* addr);

// Neither 'any' nor (on esp8266) 'none'
bool address_set(const IPAddress& ip);

// dns_gethostbyname() with the result stored into `out` right away when it is cached (ERR_OK)
err_t gethostbyname(const char* name, IPAddress& out, dns_found_callback callback, void* arg);

} // namespace chip
} // namespace espurna
