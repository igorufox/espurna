/*

RELAY MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>

*/

#pragma once

#include <Arduino.h>

#include <cstdint>
#include <memory>

#include "system.h"
#include "rpc.h"

constexpr size_t RelaysMax { 32ul };

// inline: the providers of other modules (light, the relay of a Tuya MCU ...) derive from it without being linked
// against the relay module
class RelayProviderBase {
public:
    RelayProviderBase() = default;
    virtual ~RelayProviderBase() = default;

    // whether the provider is ready
    virtual bool setup() {
        return true;
    }

    // status requested at boot
    virtual void boot(bool) {
    }

    // when 'status' was requested, but target status remains the same or is canceled
    virtual void notify(bool) {
    }

    // when relay 'status' is changed from target to current
    virtual void change(bool status) = 0;

    // unique id of the provider
    virtual espurna::StringView id() const = 0;
};

// gets either current or target status, where current is the status that we are
// actually in and target is the status we would be, eventually, unless
// relayStatus(id, relayStatus()) is called
enum class RelayStatus {
    NotAvailable = -2,
    NotReady = -1,
    Off = 0,
    On = 1,
};

RelayStatus relayStatus(size_t id);
RelayStatus relayTargetStatus(size_t id);
bool relayStatus();

// applies specific status or toggles between them
bool relayStatus(size_t id, bool status);
bool relayToggle(size_t id);

size_t relayCount();
int relayBtnGpio(size_t index);

// globally used strings for payload parsing & report
// is customizable, but for every relay at once
espurna::StringView relayPayloadOn();
espurna::StringView relayPayloadOff();
espurna::StringView relayPayloadToggle();

espurna::StringView relayPayload(PayloadStatus status);

PayloadStatus relayParsePayload(espurna::StringView);

// toggle the specified relay after N milliseconds
void relayTimer(size_t id, espurna::duration::Milliseconds);

// immediately toggle the specified relay and toggle back after N milliseconds
void relayPulse(size_t id, espurna::duration::Milliseconds);

using RelayStatusCallback = void(*)(size_t id, bool status);
using RelayProviderBasePtr = std::unique_ptr<RelayProviderBase>;

struct RelayAddResult {
    size_t id { RelaysMax };

    explicit operator bool() const {
        return id != RelaysMax;
    }
};

// the relays of a provider module that started after this one (relay.cpp)
void relayProvidersReady();

RelayAddResult relayAdd(RelayProviderBasePtr&& provider);
void relayOnStatusNotify(RelayStatusCallback);
void relayOnStatusChange(RelayStatusCallback);
void relayOnReady(RelayStatusCallback);

void relaySetupDummy(size_t size, bool reconfigure = false);
void relaySetup();

