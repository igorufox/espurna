/*

THINGSPEAK MODULE

Copyright (C) 2019 by Xose Pérez <xose dot perez at gmail dot com>

*/

#pragma once

#include <cstdint>

bool tspkEnqueueRelay(unsigned char index, const espurna::string& value);
bool tspkEnqueueMagnitude(unsigned char index, const espurna::string& value);
void tspkFlush();

bool tspkEnabled();
void tspkSetup();
