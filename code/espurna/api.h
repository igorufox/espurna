/*

API MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2020-2021 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#pragma once

#include <functional>

#include "json.h"

#include "api_path.h"
#include "api_impl.h"

namespace espurna {
namespace api {

using BasicHandler = std::function<bool(Request&)>;
// GET: the response object is open, the handler writes its members; PUT: the JSON body
using JsonHandler = std::function<bool(Request&, json::Writer& response)>;
using JsonPutHandler = std::function<bool(Request&, json::Value body)>;

} // namespace api
} // namespace espurna

using ApiRequest = espurna::api::Request;
using ApiBasicHandler = espurna::api::BasicHandler;
using ApiJsonHandler = espurna::api::JsonHandler;

void apiRegister(espurna::string path,
    espurna::api::BasicHandler&& get,
    espurna::api::BasicHandler&& put);

void apiRegister(espurna::string path,
    espurna::api::JsonHandler&& get,
    espurna::api::JsonPutHandler&& put);

// inline: the modules answer with them without being linked against the api module
inline bool apiError(espurna::api::Request& request) {
    request.send(espurna::string("ERROR"));
    return true;
}

inline bool apiOk(espurna::api::Request& request) {
    request.send(espurna::string("OK"));
    return true;
}

bool apiReservedParam(espurna::StringView);
espurna::string apiKey();
bool apiEnabled();
bool apiRestFul();
void apiCommonSetup();

void apiSetup();
