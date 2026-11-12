/*

The ws module (docs/modules.md): the WebSocket server of the WebUI is a module, the head forwards to it

The head's parts and the other modules call wsPost / wsSend / wsConnected ... as always (ws.h); the head has only the
forwarders (ws.cpp) to the table the ws module hands over when it starts (module_ws.cpp, ws_server.cpp). The callbacks
they register (wsRegister()) stay in the head, the module sends them to a client that connects. Without the module:
nobody connected, nothing sent.

*/

#pragma once

#include "ws.h"

struct WsModuleApi {
    using Container = espurna::web::ws::Callbacks::OnSendContainer;

    void (*post)(uint32_t, espurna::web::ws::OnSend&&);
    void (*postHint)(uint32_t, espurna::web::ws::OnSend&&, size_t);
    void (*postManual)(uint32_t, espurna::web::ws::OnSend&&);
    void (*postManualHint)(uint32_t, espurna::web::ws::OnSend&&, size_t);

    // a list taken by reference (a registered one, it lives as long as the board runs) or moved in
    void (*postAll)(uint32_t, Container&&);
    void (*postAllRef)(uint32_t, const Container&);
    void (*postSequence)(uint32_t, Container&&);
    void (*postSequenceRef)(uint32_t, const Container&);

    void (*sendAll)(JsonObject&);
    void (*sendTo)(uint32_t, JsonObject&);
    void (*sendText)(const char*);

    WsClientInfo (*clientInfo)(uint32_t);
    void (*pause)(bool);
    bool (*connected)();
    bool (*connectedTo)(uint32_t);

    bool (*debugSend)(const DebugPrefix&, const char*, size_t);
};

// the module's entry: its table
void modulesWsProvide(const WsModuleApi* api);
