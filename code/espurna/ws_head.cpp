/*

The WebUI parts of the head that read JSON: the ws module (the head has no JSON code). The DEBUG panel's command
line: the "cmd" action runs a terminal command, its output goes back as the log of that client.

*/

#include "espurna.h"

#if MODULE_BUILD && WEB_SUPPORT && TERMINAL_SUPPORT

#include <memory>

#include "terminal.h"
#include "terminal_commands.h"
#include "terminal_parsing.h"
#include "ws.h"
#include "ws_utils.h"

namespace espurna {
namespace terminal {
namespace web {
namespace {

STRING_VIEW_INLINE(Prefix, "cmd");

using Output = PrintLine<espurna::web::ws::InplaceLog>;

struct Command {
    espurna::string line;
    uint32_t id;
};

void onVisible(espurna::json::Writer& root) {
    wsPayloadModule(root, Prefix);
}

void onAction(uint32_t client_id, const char* action, espurna::json::Value data) {
    STRING_VIEW_INLINE(Cmd, "cmd");
    if (Cmd != action) {
        return;
    }

    STRING_VIEW_INLINE(Line, "line");
    const auto line = data[Line].asString();
    auto cmd = Command{
        .line = espurna::string(line.c_str()),
        .id = client_id,
    };

    if (!cmd.line.length()) {
        return;
    }

    const auto shared =
        std::make_shared<Command>(std::move(cmd));

    espurnaRegisterOnce([shared]() {
        wsPostManual(shared->id,
            [shared](espurna::json::Writer&) {
                Output out(shared->id);
                out.output().wait_time(espurnaLoopDelay());
                api_find_and_call(shared->line, out);
            });
    });
}

} // namespace
} // namespace web
} // namespace terminal
} // namespace espurna

// from the ws module's start
void wsHeadSetup() {
    wsRegister()
        .onVisible(espurna::terminal::web::onVisible)
        .onAction(espurna::terminal::web::onAction);
}

#else

void wsHeadSetup() {
}

#endif // MODULE_BUILD && WEB_SUPPORT && TERMINAL_SUPPORT
