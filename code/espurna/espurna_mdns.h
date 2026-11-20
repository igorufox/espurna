/*

MDNS MODULE

Copyright (C) 2017-2019 by Xose Pérez <xose dot perez at gmail dot com>

*/

#pragma once


#include <cstdint>

using MdnsServerQueryCallback = bool(*)(espurna::string&& server, uint16_t port);
bool mdnsServiceQuery(const espurna::string& service, const espurna::string& protocol, MdnsServerQueryCallback callback);

bool mdnsRunning();
void mdnsServerSetup();

