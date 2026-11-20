/*

RF BRIDGE MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>

*/

#pragma once

#include <cstdint>

#include "types.h"

using RfbCodeHandler = void(*)(unsigned char protocol, espurna::StringView code);
void rfbOnCode(RfbCodeHandler);

void rfbSend(espurna::StringView code);

void rfbStatus(size_t id, bool status);
void rfbLearn(size_t id, bool status);

espurna::string rfbRetrieve(size_t id, bool status);
void rfbStore(size_t id, bool status, espurna::string code);

void rfbForget(size_t id, bool status);
void rfbSetup();
