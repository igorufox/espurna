/*

NETWORKING MODULE

Copyright (C) 2022 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#pragma once

#include "ip4.h"

#include <memory>

#include <lwip/init.h>
#include <lwip/err.h>

#include "types.h"

namespace espurna {
namespace network {
namespace dns {

struct Host {
    espurna::string name;
    espurna::Ip4 addr;
    err_t err;
};

using HostPtr = std::shared_ptr<Host>;
using HostCallback = std::function<void(HostPtr)>;

// DNS request is lauched in the background, HostPtr should be waited upon
HostPtr resolve(espurna::string);

// ...or, user callback is executed when DNS client is ready to return something
void resolve(espurna::string, HostCallback);

// Block until the HostPtr becomes available for reading, or when timeout occurs
bool wait_for(HostPtr, duration::Milliseconds);

// Arduino style result
espurna::Ip4 gethostbyname(espurna::string, duration::Milliseconds);
espurna::Ip4 gethostbyname(espurna::string);

} // namespace dns
} // namespace network
} // namespace espurna

void networkSetup();

