/*

THE HEAD AND THE MODULES (docs/modules.md): what the head image of every chip has, one list for ESP8266, ESP32 and Beken

The head (MODULES_SUPPORT=1 from platformio_<chip>.ini): Wi-Fi, web + WebUI, API, terminal + telnet, NTP, OTA, relays,
buttons, LEDs, the module loader, and the forwarders of the MQTT API (modules_mqtt.h, MQTT_MODULE): its parts call MQTT
as always, the mqtt module takes over when it is installed and switched on. Everything else is a module, switched off
in the head here.

The module objects (MODULE_BUILD=1 from custom_module_flags of scripts/module_link.py): no loader, the module features
on. The modules themselves are one list for every chip, ESPURNA_MODULES below. The ini keeps only what depends on the
chip: its own objects of a module and the modules of that list it does not build (custom_modules,
custom_modules_skip: only what cannot exist on it, Matter and the NSPanel on the ESP8266, the NSPanel on Beken). The
features of the modules are set here for every chip alike.

Included before hardware.h / general.h: their #ifndef defaults do not apply to what is set here.

*/

#pragma once

/* ESPURNA_MODULES: every module of every chip (scripts/module_link.py reads the lines between the two markers)

   <name> | <objects> | <objects only for what the head does not have> | <functions it provides to other modules>
       | <modules it needs>

   The chip's ini adds its own objects to a module (custom_modules: a line of the same name, its objects go first),
   its own modules (a line of a new name) and leaves out the modules it does not build (custom_modules_skip).

sensors | sensor.cpp.o module_sensors.cpp.o | i2c.cpp.o driver_onewire.cpp.o uart.cpp.o | magnitudeCount magnitudeInfo magnitudeTypeTopic magnitudeUnitsName sensorReady sensorWebSocketMagnitudes sensorOnMagnitudeRead sensorOnMagnitudeReport magnitudeValue magnitudeReportValue magnitudeReadValue magnitudeType magnitudeIndex magnitudeTopic
scheduler | scheduler.cpp.o module_scheduler.cpp.o | |
alexa | alexa.cpp.o module_alexa.cpp.o | | alexaPause
mdns | mdns.cpp.o module_mdns.cpp.o | | mdnsRunning mdnsServiceQuery
mqtt | mqtt.cpp.o module_mqtt.cpp.o | | mqttHeartbeat mqttRegister mqttOnPublish mqttOnSubscribe mqttTopic mqttTopicSetter mqttMagnitude mqttSendRaw mqttSubscribeRaw mqttSubscribe mqttUnsubscribeRaw mqttUnsubscribe mqttSend mqttSendStatus mqttFlush mqttEnqueue mqttPayloadOnline mqttPayloadOffline mqttPayloadStatus mqttEnabled mqttForward mqttConnected mqttDisconnect mqttSetup
homeassistant | homeassistant.cpp.o module_homeassistant.cpp.o | | | mqtt
domoticz | domoticz.cpp.o module_domoticz.cpp.o | | domoticzSendMagnitude | mqtt
thingspeak | thingspeak.cpp.o module_thingspeak.cpp.o | | tspkEnqueueMagnitude
light | light.cpp.o module_light.cpp.o | pwm.cpp.o | lightBlue lightBrightness lightBrightnessPercent lightChannel lightChannels lightColdWhite lightColor lightGreen lightHasColdWhite lightHasColor lightHasWarmWhite lightHasWhite lightHs lightHsv lightMiredsRange lightOnReport lightRed lightRgb lightState lightTemperature lightTransitionStep lightTransitionTime lightUpdate lightUseRGB lightWarmWhite
rf | rfbridge.cpp.o module_rf.cpp.o | |
matter | matter.cpp.o module_matter.cpp.o | |
nspanel | nspanel.cpp.o module_nspanel.cpp.o | |
captive | captive.cpp.o module_captive.cpp.o | |

ESPURNA_MODULES */

// Modules on out of the box (names, space separated): the head image carries them switched on, the full image places
// them switched on; everything else is off until the user switches it on. Read by the image scripts of every chip
// (scripts/<chip>_full_image.py, a regex on this line)
#define MODULES_ON_BY_DEFAULT       "captive"

#ifndef MODULE_BUILD
#define MODULE_BUILD                0
#endif

#if MODULE_BUILD

// --- the module objects: the module's part on, the loader and the forwarders are the head's

#undef MODULES_SUPPORT
#define MODULES_SUPPORT             0

#undef MQTT_MODULE
#define MQTT_MODULE                 0

#define MQTT_SUPPORT                1
#define MDNS_SERVER_SUPPORT         1
#define SCHEDULER_SUPPORT           1
#define HOMEASSISTANT_SUPPORT       1
#define ALEXA_SUPPORT               1
#define DOMOTICZ_SUPPORT            1
#define THINGSPEAK_SUPPORT          1

// the same modules on every chip: sensors with every driver, light with every provider (PWM: the chip's driver,
// general.h PWM_PROVIDER), RF (the chip's provider, general.h RFB_PROVIDER)
#define HLW8012_SUPPORT             1
#define CSE7766_SUPPORT             1
#define V9261F_SUPPORT              1
#define ECH1560_SUPPORT             1
#define PZEM004T_SUPPORT            1
#define PZEM004TV30_SUPPORT         1
#define ADE7953_SUPPORT             1
#define DHT_SUPPORT                 1
#define DALLAS_SUPPORT              1
#define BMX280_SUPPORT              1
#define ANALOG_SUPPORT              1
#define NTC_SUPPORT                 1
#define DIGITAL_SUPPORT             1
#define DUMMY_SENSOR_SUPPORT        1
#define LIGHT_PROVIDER              LIGHT_PROVIDER_ANY
#define RFB_SUPPORT                 1

// what exists only on some chips: Matter (ESP32 and Beken, there is no Matter SDK for the ESP8266), the NSPanel (an
// ESP32 device); the other chips leave the module out (custom_modules_skip)
#if defined(ESP32) || defined(LIBRETINY)
#define MATTER_SUPPORT              1
#endif
#if defined(ESP32)
#define NSPANEL_SUPPORT             1
#endif

#elif defined(MODULES_SUPPORT) && MODULES_SUPPORT

// --- the head

#define WEB_SUPPORT                 1
#define API_SUPPORT                 1
#define TERMINAL_SUPPORT            1
#define TELNET_SUPPORT              1
#define NTP_SUPPORT                 1

// MQTT: forwarders only, the client is the mqtt module
#define MQTT_SUPPORT                1
#define MQTT_MODULE                 1

// modules
#define MDNS_SERVER_SUPPORT         0
#define SCHEDULER_SUPPORT           0
#define HOMEASSISTANT_SUPPORT       0
#define ALEXA_SUPPORT               0
#define DOMOTICZ_SUPPORT            0
#define THINGSPEAK_SUPPORT          0
#define SENSOR_SUPPORT              0
#define PWM_SUPPORT                 0
#define RFB_SUPPORT                 0
#define MATTER_SUPPORT              0
#define NSPANEL_SUPPORT             0

#endif
