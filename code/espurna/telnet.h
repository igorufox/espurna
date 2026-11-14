/*

TELNET MODULE

Copyright (C) 2017-2019 by Xose Pérez <xose dot perez at gmail dot com>

*/

#pragma once

#include "types.h"

bool telnetDebugSend(const DebugPrefix&, const char* message, size_t length);

uint16_t telnetPort();
bool telnetConnected();
// a session out to `host`:`port` (the telnet_reverse module); false - no free client, or it could not even start
bool telnetReverse(const char* host, uint16_t port);
void telnetSetup();

