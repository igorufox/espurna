/*

WEBSERVER MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>

The server is the one HTTP server of ESPurna (libs/http_server.h, lwIP, every chip). Its handlers run in the loop:
nothing here waits for a lock, nothing is answered "busy".

*/

#pragma once

#include "espurna.h"

#include "libs/http_server.h"

#include <functional>
#include <vector>

#include "web_utils.h"
#include "web_print.h"

using WebRequest = espurna::http::server::Request;
using WebMethod = espurna::http::server::Method;
using WebServer = espurna::http::server::Server;

// a body that no route took (alexa): data, its length, its offset in the body, the whole body length. true - taken
using web_body_callback_f = std::function<bool(WebRequest&, const uint8_t* data, size_t len, size_t index, size_t total)>;
// a request that no route took (alexa, prometheus, terminal). true - answered
using web_request_callback_f = std::function<bool(WebRequest&)>;

uint16_t webPort();
WebServer& webServer();

// the request came to the access point by its address or the hostname
bool webApModeRequest(WebRequest&);

// the login of the WebUI user (admin + adminPass), Basic or Digest
bool webAuthenticate(WebRequest&);
// 401 with the Digest challenge (realm = the hostname)
void webRequestAuthentication(WebRequest&);

void webSecurityHeaders(WebRequest&);
void webLog(WebRequest&);

// the Arduino side of a request
String webHeader(WebRequest&, const char* name);
String webParam(WebRequest&, const char* name);
void webSend(WebRequest&, int code, const char* type, const String& body);
void webSend(WebRequest&, int code);

void webBodyRegister(web_body_callback_f);
void webRequestRegister(web_request_callback_f);

void webSetup();
