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

#include "text_types.h"


size_t crashReservedSize();

void crashResetReason(::espurna::Output&);
void crashForceDump(::espurna::Output&);
void crashDump(::espurna::Output&);
void crashClear();

void crashSetup();

// The last crash in short, for the banner of the modules page (modules.cpp): what it was, and the code addresses it
// went through (the page names the module one of them is in). false - no crash record
struct CrashCause {
    char text[88];
    uint32_t addresses[16];
    size_t count;
};

bool crashCause(CrashCause&);

// Chip part, arch/<chip>/system_<chip>.cpp. The common record in the EEPROM is written by the esp8266 Core
// crash handler (custom_crash_callback()); other chips keep what they know elsewhere
namespace espurna {
namespace debug {
namespace crash {
namespace chip {

// Crash record of the chip itself, instead of the EEPROM one (Beken flash record). true when the chip has one,
// `check` only asks for a crash that was not shown yet
bool dump(::espurna::Output&, bool check);

// EEPROM record is empty, what the chip knows about the last reset instead (ESP32 core dump). true when printed
bool dump_empty(::espurna::Output&);

// The chip's own record in short (ESP32 core dump, Beken flash record). true when the chip keeps its own record, even
// if empty (then `text` stays empty); false - the EEPROM one of crash.cpp (ESP8266)
bool cause(CrashCause&);

// After the EEPROM record was checked at boot
void setup();

} // namespace chip
} // namespace crash
} // namespace debug
} // namespace espurna

