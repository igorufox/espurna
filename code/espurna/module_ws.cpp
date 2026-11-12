/*

The ws module (docs/modules.md, modules_ws.h), every chip alike: the WebSocket of the WebUI (ws_server.cpp). It hands
its API over to the head; the head's parts and the other modules call the head's forwarders. The WebUI module needs
it. Starts first: a client may connect as soon as the web server answers.

*/

#include "espurna.h"

#if MODULE_BUILD && WEB_SUPPORT

#include "modules.h"
#include "modules_ws.h"

namespace {

using OnSend = espurna::web::ws::OnSend;
using Container = WsModuleApi::Container;

// the module's own wsPost ... (renamed in this module, the provides column of config/head.h)
const WsModuleApi Table {
    .post = static_cast<void(*)(uint32_t, OnSend&&)>(wsPost),
    .postHint = static_cast<void(*)(uint32_t, OnSend&&, size_t)>(wsPost),
    .postManual = static_cast<void(*)(uint32_t, OnSend&&)>(wsPostManual),
    .postManualHint = static_cast<void(*)(uint32_t, OnSend&&, size_t)>(wsPostManual),

    .postAll = static_cast<void(*)(uint32_t, Container&&)>(wsPostAll),
    .postAllRef = static_cast<void(*)(uint32_t, const Container&)>(wsPostAll),
    .postSequence = static_cast<void(*)(uint32_t, Container&&)>(wsPostSequence),
    .postSequenceRef = static_cast<void(*)(uint32_t, const Container&)>(wsPostSequence),

    .sendAll = static_cast<void(*)(JsonObject&)>(wsSend),
    .sendTo = static_cast<void(*)(uint32_t, JsonObject&)>(wsSend),
    .sendText = static_cast<void(*)(const char*)>(wsSend),

    .clientInfo = wsClientInfo,
    .pause = wsPause,
    .connected = static_cast<bool(*)()>(wsConnected),
    .connectedTo = static_cast<bool(*)(uint32_t)>(wsConnected),

#if DEBUG_WEB_SUPPORT
    .debugSend = wsDebugSend,
#else
    .debugSend = nullptr,
#endif
};

} // namespace

ESPURNA_MODULE(ws, "ws", 2) {
    wsSetup();
    modulesWsProvide(&Table);
}

#endif
