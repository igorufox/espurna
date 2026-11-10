/*

Part of the DEBUG MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2019-2020 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

// -----------------------------------------------------------------------------
// Save crash info
// Original code by @krzychb
// https://github.com/krzychb/EspSaveCrash
// -----------------------------------------------------------------------------

#pragma once

#include <Arduino.h>

size_t crashReservedSize();

void crashResetReason(Print&);
void crashForceDump(Print&);
void crashDump(Print&);
void crashClear();

void crashSetup();

// Chip part, arch/<chip>/system_<chip>.cpp. The common record in the EEPROM is written by the esp8266 Core
// crash handler (custom_crash_callback()); other chips keep what they know elsewhere
namespace espurna {
namespace debug {
namespace crash {
namespace chip {

// Crash record of the chip itself, instead of the EEPROM one (Beken flash record). true when the chip has one,
// `check` only asks for a crash that was not shown yet
bool dump(Print&, bool check);

// EEPROM record is empty, what the chip knows about the last reset instead (ESP32 core dump). true when printed
bool dump_empty(Print&);

// After the EEPROM record was checked at boot
void setup();

} // namespace chip
} // namespace crash
} // namespace debug
} // namespace espurna

