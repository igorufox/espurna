/*

PROTOTYPE: the light module (docs/modules.md), env:esp8266-1m-modules of platformio_esp8266.ini

*/

#include "espurna.h"

#if MODULE_BUILD && (LIGHT_PROVIDER != LIGHT_PROVIDER_NONE)

#include "modules.h"
#include "light.h"

ESPURNA_MODULE(light, "light", 10) {
    using espurna::modules::Api;

    // the real ones (renamed in this module), before the integrations call them
    modulesProvide(Api::LightStateSet, static_cast<void(*)(bool)>(&lightState));
    modulesProvide(Api::LightStateIdSet, static_cast<void(*)(size_t, bool)>(&lightState));
    modulesProvide(Api::LightStateGet, static_cast<bool(*)()>(&lightState));
    modulesProvide(Api::LightUpdate, static_cast<void(*)()>(&lightUpdate));
    modulesProvide(Api::LightUpdateTransition, static_cast<void(*)(LightTransition)>(&lightUpdate));
    modulesProvide(Api::LightChannelGet, static_cast<long(*)(size_t)>(&lightChannel));
    modulesProvide(Api::LightChannelSet, static_cast<void(*)(size_t, long)>(&lightChannel));
    modulesProvide(Api::LightChannels, static_cast<size_t(*)()>(&lightChannels));
    modulesProvide(Api::LightOnReport, static_cast<void(*)(LightReportListener)>(&lightOnReport));
    modulesProvide(Api::LightColor, static_cast<String(*)()>(&lightColor));
    modulesProvide(Api::LightUseRgb, static_cast<bool(*)()>(&lightUseRGB));
    modulesProvide(Api::LightHasColor, static_cast<bool(*)()>(&lightHasColor));
    modulesProvide(Api::LightHasWhite, static_cast<bool(*)()>(&lightHasWhite));
    modulesProvide(Api::LightHasColdWhite, static_cast<bool(*)()>(&lightHasColdWhite));
    modulesProvide(Api::LightHasWarmWhite, static_cast<bool(*)()>(&lightHasWarmWhite));
    modulesProvide(Api::LightColdWhiteSet, static_cast<void(*)(long)>(&lightColdWhite));
    modulesProvide(Api::LightColdWhiteGet, static_cast<long(*)()>(&lightColdWhite));
    modulesProvide(Api::LightWarmWhiteSet, static_cast<void(*)(long)>(&lightWarmWhite));
    modulesProvide(Api::LightWarmWhiteGet, static_cast<long(*)()>(&lightWarmWhite));
    modulesProvide(Api::LightBrightnessSet, static_cast<void(*)(long)>(&lightBrightness));
    modulesProvide(Api::LightBrightnessGet, static_cast<long(*)()>(&lightBrightness));
    modulesProvide(Api::LightBrightnessPercent, static_cast<void(*)(long)>(&lightBrightnessPercent));
    modulesProvide(Api::LightMiredsRange, static_cast<espurna::light::TemperatureRange(*)()>(&lightMiredsRange));
    modulesProvide(Api::LightTemperatureMireds, static_cast<void(*)(espurna::light::Mireds)>(&lightTemperature));
    modulesProvide(Api::LightTransitionStep, static_cast<espurna::duration::Milliseconds(*)()>(&lightTransitionStep));
    modulesProvide(Api::LightTransitionTime, static_cast<espurna::duration::Milliseconds(*)()>(&lightTransitionTime));
    modulesProvide(Api::LightHs, static_cast<void(*)(long, long)>(&lightHs));
    modulesProvide(Api::LightHsvGet, static_cast<espurna::light::Hsv(*)()>(&lightHsv));
    modulesProvide(Api::LightRgbSet, static_cast<void(*)(espurna::light::Rgb)>(&lightRgb));
    modulesProvide(Api::LightRgbGet, static_cast<espurna::light::Rgb(*)()>(&lightRgb));
    modulesProvide(Api::LightRedSet, static_cast<void(*)(long)>(&lightRed));
    modulesProvide(Api::LightGreenSet, static_cast<void(*)(long)>(&lightGreen));
    modulesProvide(Api::LightBlueSet, static_cast<void(*)(long)>(&lightBlue));

    lightSetup();
}

#endif
