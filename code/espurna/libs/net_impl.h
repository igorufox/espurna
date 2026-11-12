/*

NET - the implementations of the one network stack of everything ESPurna builds, included by exactly ONE .cpp of an
image: ESPurna's net.cpp (the head of every chip), code/mini-app's and code/safeboot-app's main.cpp.
- libs/tcp.h            the TCP connection and listener on lwIP's raw API
- libs/http_client.h    the HTTP client (BearSSL TLS)
- libs/http_server.h    the HTTP server
- libs/websocket.h      the WebSocket server (an upgraded request of the HTTP server)

What that .cpp defines before the include is only how the image logs and locks, never what the stack does:
    NET_LOCK            the lock between lwIP's callbacks and the loop (lock(), unlock()); a no-op one without an OS
    NET_LOG(fmt, ...)   one log line (fmt a string literal)
    HTTP_CLIENT_AGENT   the User-Agent of the client

*/

#pragma once

#include "tcp_impl.h"
#include "http_client_impl.h"
#include "http_server_impl.h"
#include "websocket_impl.h"
