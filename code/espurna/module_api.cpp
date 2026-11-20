/*

The api module (docs/modules.md): the HTTP API (/api/..., the apiKey, REST and RPC; api.cpp, api_common.cpp). Out
of the head (owner 2026-11-12): nothing of the head needs it, the head page and the recovery have their own pages.
Starts before the modules that register API paths (relay, sensors, light, scheduler, rf), which call apiRegister()
through the head: they work without the API too.

*/

#if MODULE_BUILD

#include "espurna.h"
#include "modules.h"
#include "api.h"

#if WEB_SUPPORT
// its part of the WebUI (html/parts.mjs, gulp build)
#include "static/ui_api.ipp"
#endif

ESPURNA_MODULE(api, "api", 4) {
#if WEB_SUPPORT
    modulesUi("api", ui_api_data, sizeof(ui_api_data));
#endif

    using espurna::modules::Api;

    modulesProvide(Api::ApiRegisterBasic, static_cast<void(*)(espurna::string, espurna::api::BasicHandler&&,
        espurna::api::BasicHandler&&)>(&apiRegister));
    modulesProvide(Api::ApiRegisterJson, static_cast<void(*)(espurna::string, espurna::api::JsonHandler&&,
        espurna::api::JsonPutHandler&&)>(&apiRegister));

    apiCommonSetup();
    apiSetup();
}

#endif // MODULE_BUILD
