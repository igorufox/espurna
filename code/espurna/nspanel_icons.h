/*

NSPANEL MODULE - icon table

Lovelace UI TFT icon font is the Material Design Icons font remapped into the BMP private use area:
    TFT codepoint = MDI codepoint - 0xF0001 + 0xE000
Values are from joBr99/nspanel-lovelace-ui HMI/code_gen/icons/icons.json (2023-06 state).
Any other icon can be used in the config as a raw codepoint, e.g. "E334" or "U+E334".

*/

#pragma once

#include <cstdint>

namespace espurna {
namespace nspanel {
namespace icons {

struct Icon {
    const char* name;
    uint16_t code;
};

static constexpr uint16_t AlertCircleOutline { 0xE5D5 };
static constexpr uint16_t ArrowLeftBold { 0xE730 };
static constexpr uint16_t ArrowRightBold { 0xE733 };
static constexpr uint16_t Home { 0xE2DB };
static constexpr uint16_t Thermometer { 0xE50E };
static constexpr uint16_t WifiOff { 0xE5A9 };
static constexpr uint16_t LightSwitch { 0xE97D };
static constexpr uint16_t GestureTapButton { 0xF2A7 };
static constexpr uint16_t LightbulbGroup { 0xF252 };

static constexpr uint16_t WeatherSunny { 0xE598 };
static constexpr uint16_t WeatherPartlyCloudy { 0xE594 };
static constexpr uint16_t WeatherCloudy { 0xE58F };
static constexpr uint16_t WeatherRainy { 0xE596 };
static constexpr uint16_t WeatherPouring { 0xE595 };
static constexpr uint16_t WeatherSnowy { 0xE597 };
static constexpr uint16_t WeatherSnowyRainy { 0xE67E };
static constexpr uint16_t WeatherFog { 0xE590 };
static constexpr uint16_t WeatherHail { 0xE591 };
static constexpr uint16_t WeatherLightning { 0xE592 };
static constexpr uint16_t WeatherLightningRainy { 0xE67D };
static constexpr uint16_t WeatherNight { 0xE593 };
static constexpr uint16_t WeatherNightPartlyCloudy { 0xEF30 };

static const Icon Table[] {
    {"alert-circle-outline", 0xE5D5},
    {"arrow-down", 0xE044},
    {"arrow-left-bold", 0xE730},
    {"arrow-right-bold", 0xE733},
    {"arrow-up", 0xE05C},
    {"arrow-up-bold", 0xE736},
    {"ceiling-light", 0xE768},
    {"ceiling-light-outline", 0xF7C6},
    {"coach-lamp", 0xF01F},
    {"desk-lamp", 0xE95E},
    {"door", 0xE819},
    {"door-open", 0xE81B},
    {"fan", 0xE20F},
    {"fan-off", 0xE81C},
    {"fire", 0xE237},
    {"flash", 0xE240},
    {"floor-lamp", 0xE8DC},
    {"flower", 0xE249},
    {"fountain", 0xE96A},
    {"garage", 0xE6D8},
    {"garage-open", 0xE6D9},
    {"garage-variant", 0xF2D2},
    {"gate", 0xE298},
    {"gate-open", 0xF169},
    {"gesture-tap-button", 0xF2A7},
    {"heat-wave", 0xFA44},
    {"home", 0xE2DB},
    {"home-floor-1", 0xED7F},
    {"home-floor-2", 0xED80},
    {"home-outline", 0xE6A0},
    {"lamp", 0xE6B4},
    {"lamps", 0xF575},
    {"led-strip-variant", 0xF050},
    {"light-switch", 0xE97D},
    {"lightbulb", 0xE334},
    {"lightbulb-group", 0xF252},
    {"lightbulb-group-off", 0xF2CC},
    {"lightbulb-group-off-outline", 0xF2CD},
    {"lightbulb-off", 0xEE4E},
    {"lightbulb-off-outline", 0xEE4F},
    {"lightbulb-on", 0xE6E7},
    {"lightbulb-on-outline", 0xE6E8},
    {"lightbulb-outline", 0xE335},
    {"lock", 0xE33D},
    {"lock-open", 0xE33E},
    {"outdoor-lamp", 0xF053},
    {"pool", 0xE605},
    {"power", 0xE424},
    {"power-plug", 0xE6A4},
    {"power-plug-off", 0xE6A5},
    {"power-socket-eu", 0xE7E6},
    {"radiator", 0xE437},
    {"spotlight", 0xE4C7},
    {"sprinkler", 0xF05E},
    {"sprinkler-variant", 0xF05F},
    {"stairs", 0xE4CC},
    {"stop", 0xE4DA},
    {"string-lights", 0xF2B9},
    {"string-lights-off", 0xF2BA},
    {"television", 0xE501},
    {"temperature-celsius", 0xE503},
    {"thermometer", 0xE50E},
    {"toggle-switch", 0xE520},
    {"toggle-switch-off", 0xE521},
    {"track-light", 0xE913},
    {"tree", 0xE530},
    {"wall-sconce", 0xE91B},
    {"wall-sconce-flat", 0xE91C},
    {"wall-sconce-round", 0xE747},
    {"wall-sconce-round-variant", 0xE91D},
    {"water-percent", 0xE58D},
    {"water-pump", 0xE58E},
    {"water-pump-off", 0xEF92},
    {"weather-cloudy", 0xE58F},
    {"weather-fog", 0xE590},
    {"weather-hail", 0xE591},
    {"weather-lightning", 0xE592},
    {"weather-lightning-rainy", 0xE67D},
    {"weather-night", 0xE593},
    {"weather-night-partly-cloudy", 0xEF30},
    {"weather-partly-cloudy", 0xE594},
    {"weather-pouring", 0xE595},
    {"weather-rainy", 0xE596},
    {"weather-snowy", 0xE597},
    {"weather-snowy-rainy", 0xE67E},
    {"weather-sunny", 0xE598},
    {"weather-windy", 0xE59C},
    {"weather-windy-variant", 0xE59D},
    {"wifi", 0xE5A8},
    {"wifi-off", 0xE5A9},
    {"window-shutter", 0xF11B},
    {"window-shutter-open", 0xF11D},
};

// RGB565 decimal, as the TFT expects them (AppDaemon luibackend values)
namespace colors {

static constexpr uint16_t On { 65222 };
static constexpr uint16_t Off { 17299 };
static constexpr uint16_t Unknown { 16904 };
static constexpr uint16_t White { 65535 };
static constexpr uint16_t Red { 63488 };
static constexpr uint16_t Background { 6371 };

static constexpr uint16_t Sunny { 65504 };
static constexpr uint16_t PartlyCloudy { 38066 };
static constexpr uint16_t ClearNight { 38060 };
static constexpr uint16_t Cloudy { 31728 };
static constexpr uint16_t Fog { 38066 };
static constexpr uint16_t Rainy { 25375 };
static constexpr uint16_t Pouring { 12703 };
static constexpr uint16_t Snowy { 65535 };
static constexpr uint16_t SnowyRainy { 38079 };
static constexpr uint16_t Lightning { 65120 };
static constexpr uint16_t LightningRainy { 50400 };
static constexpr uint16_t Exceptional { 63878 };

} // namespace colors

struct WeatherIcon {
    uint16_t icon;
    uint16_t color;
};

// WMO weather interpretation codes, as used by Open-Meteo
inline WeatherIcon weather(int code, bool day) {
    switch (code) {
    case 0:
        return day
            ? WeatherIcon{WeatherSunny, colors::Sunny}
            : WeatherIcon{WeatherNight, colors::ClearNight};
    case 1:
    case 2:
        return day
            ? WeatherIcon{WeatherPartlyCloudy, colors::PartlyCloudy}
            : WeatherIcon{WeatherNightPartlyCloudy, colors::ClearNight};
    case 3:
        return {WeatherCloudy, colors::Cloudy};
    case 45:
    case 48:
        return {WeatherFog, colors::Fog};
    case 51:
    case 53:
    case 55:
    case 61:
    case 63:
    case 80:
    case 81:
        return {WeatherRainy, colors::Rainy};
    case 65:
    case 82:
        return {WeatherPouring, colors::Pouring};
    case 56:
    case 57:
    case 66:
    case 67:
        return {WeatherSnowyRainy, colors::SnowyRainy};
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86:
        return {WeatherSnowy, colors::Snowy};
    case 95:
        return {WeatherLightningRainy, colors::LightningRainy};
    case 96:
    case 99:
        return {WeatherHail, colors::Snowy};
    }

    return {AlertCircleOutline, colors::Exceptional};
}

} // namespace icons
} // namespace nspanel
} // namespace espurna
