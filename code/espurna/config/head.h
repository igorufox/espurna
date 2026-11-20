/*

THE HEAD AND THE MODULES (docs/modules.md): what the head image of every chip has, one list for ESP8266, ESP32 and Beken

The head (MODULES_SUPPORT=1 from platformio_<chip>.ini): Wi-Fi, the web server (the WebSocket and the WebUI are
modules: ws, webui), terminal + telnet, OTA, the module loader, and the forwarders of the APIs of the modules it and
the modules call (modules_api.cpp, the MQTT API: modules_mqtt.h, MQTT_MODULE): its parts call MQTT as always, the mqtt
module takes over when it is installed and switched on. Everything else is a module, switched off in the head here:
the relays, buttons and LEDs (relay, button, led: on by default in the initial images), the HTTP API (api), NTP (ntp).

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
       | <modules it needs> | <bytes of flash it keeps its data in: modulesData(), placed by the Apply>

   The sensor drivers: one module each (sensors/module_<name>.cpp, sensors/driver.h), all of them need the sensors
   module (the manager) and start after it; the UART ones need the uart module too, the I2C ones the i2c module (the
   bus and the one Wire, no I2C in the head).

   A module is linked against the modules it needs (the last column) directly, as against the head
   (scripts/module_link.py): it calls them, the Apply writes them before it and puts their addresses into it.

   The chip's ini adds its own objects to a module (custom_modules: a line of the same name, its objects go first),
   its own modules (a line of a new name) and leaves out the modules it does not build (custom_modules_skip).

tools | json.cpp.o ws_utils.cpp.o datetime.cpp.o module_tools.cpp.o | |
arduino | module_arduino.cpp.o | |
ws | ws_server.cpp.o settings_json.cpp.o ws_head.cpp.o module_ws.cpp.o | | wsPost wsPostManual wsPostAll wsPostSequence wsSend wsClientInfo wsPause wsConnected wsDebugSend | tools
api | api.cpp.o api_common.cpp.o module_api.cpp.o | | apiRegister| tools
ntp | ntp.cpp.o ntp_driver.cpp.o module_ntp.cpp.o | | ntpOnTick ntpDateTime ntpSynced| tools
relay | relay.cpp.o module_relay.cpp.o | | relayCount relayStatus relayToggle relayParsePayload relayPayload relayOnStatusChange relayOnStatusNotify relayOnReady relayBtnGpio| tools
button | button.cpp.o module_button.cpp.o | DebounceEvent.cpp.o ac_switch.cpp.o | buttonSetCustomAction| tools
led | led.cpp.o module_led.cpp.o | || tools
i2c | i2c.cpp.o module_i2c.cpp.o | || tools arduino
sensors | sensor.cpp.o module_sensors.cpp.o | | magnitudeCount magnitudeInfo magnitudeTypeTopic magnitudeUnitsName sensorReady sensorWebSocketMagnitudes sensorOnMagnitudeRead sensorOnMagnitudeReport magnitudeValue magnitudeReportValue magnitudeReadValue magnitudeType magnitudeIndex magnitudeTopic| tools
a02yyu | sensors/module_a02yyu.cpp.o | | | sensors uart
am2320 | sensors/module_am2320.cpp.o | | | sensors i2c
analog | sensors/module_analog.cpp.o | | | sensors
bh1750 | sensors/module_bh1750.cpp.o | | | sensors i2c
bmp180 | sensors/module_bmp180.cpp.o | | | sensors i2c
bmx280 | sensors/module_bmx280.cpp.o | | | sensors i2c
bme680 | sensors/module_bme680.cpp.o | | | sensors i2c arduino
cse7766 | sensors/module_cse7766.cpp.o | | | sensors uart
dallas | sensors/module_dallas.cpp.o driver_onewire.cpp.o | | | sensors arduino
dht | sensors/module_dht.cpp.o | | | sensors
digital | sensors/module_digital.cpp.o | | | sensors
dummy | sensors/module_dummy.cpp.o | | | sensors
ech1560 | sensors/module_ech1560.cpp.o | | | sensors
emon_adc121 | sensors/module_emon_adc121.cpp.o | | | sensors i2c
emon_ads1x15 | sensors/module_emon_ads1x15.cpp.o | | | sensors i2c
emon_analog | sensors/module_emon_analog.cpp.o | | | sensors
events | sensors/module_events.cpp.o | | | sensors
ezoph | sensors/module_ezoph.cpp.o | | | sensors uart
geiger | sensors/module_geiger.cpp.o | | | sensors
guvas12sd | sensors/module_guvas12sd.cpp.o | | | sensors
hlw8012 | sensors/module_hlw8012.cpp.o | | | sensors arduino
ina219 | sensors/module_ina219.cpp.o | | | sensors i2c
ldr | sensors/module_ldr.cpp.o | | | sensors
max6675 | sensors/module_max6675.cpp.o | | | sensors arduino
mhz19 | sensors/module_mhz19.cpp.o | | | sensors uart
mics2710 | sensors/module_mics2710.cpp.o | | | sensors
mics5525 | sensors/module_mics5525.cpp.o | | | sensors
ntc | sensors/module_ntc.cpp.o | | | sensors
pm1006 | sensors/module_pm1006.cpp.o | | | sensors uart
pmsx003 | sensors/module_pmsx003.cpp.o | | | sensors uart
pulsemeter | sensors/module_pulsemeter.cpp.o | | | sensors
pzem004t | sensors/module_pzem004t.cpp.o | | | sensors uart
senseair | sensors/module_senseair.cpp.o | | | sensors uart
sds011 | sensors/module_sds011.cpp.o | | | sensors uart
sht3x | sensors/module_sht3x.cpp.o | | | sensors i2c
si7021 | sensors/module_si7021.cpp.o | | | sensors i2c
sm300d2 | sensors/module_sm300d2.cpp.o | | | sensors uart
sonar | sensors/module_sonar.cpp.o | | | sensors arduino
t6613 | sensors/module_t6613.cpp.o | | | sensors uart
tmp3x | sensors/module_tmp3x.cpp.o | | | sensors
v9261f | sensors/module_v9261f.cpp.o | | | sensors uart
veml6075 | sensors/module_veml6075.cpp.o | | | sensors i2c arduino
vl53l1x | sensors/module_vl53l1x.cpp.o | | | sensors i2c arduino
ade7953 | sensors/module_ade7953.cpp.o | | | sensors i2c
si1145 | sensors/module_si1145.cpp.o | | | sensors i2c arduino
hdc1080 | sensors/module_hdc1080.cpp.o | | | sensors i2c
pzem004tv30 | sensors/module_pzem004tv30.cpp.o | | | sensors uart
ble | ble.cpp.o module_ble.cpp.o | || tools
ble_wifi | ble_wifi.cpp.o module_ble_wifi.cpp.o | || ble tools
ble_proxy | ble_proxy.cpp.o module_ble_proxy.cpp.o | || ble mqtt tools
ble_sensors | sensors/module_ble_sensors.cpp.o | | | sensors ble
scheduler | scheduler.cpp.o module_scheduler.cpp.o | | | ntp tools
alexa | alexa.cpp.o module_alexa.cpp.o | | alexaPause| tools arduino
mdns | mdns.cpp.o module_mdns.cpp.o | | mdnsRunning mdnsServiceQuery| tools
mqtt | mqtt.cpp.o module_mqtt.cpp.o | | mqttHeartbeat mqttRegister mqttOnPublish mqttOnSubscribe mqttTopic mqttTopicSetter mqttMagnitude mqttSendRaw mqttSubscribeRaw mqttSubscribe mqttUnsubscribeRaw mqttUnsubscribe mqttSend mqttSendStatus mqttFlush mqttEnqueue mqttPayloadOnline mqttPayloadOffline mqttPayloadStatus mqttEnabled mqttForward mqttConnected mqttDisconnect mqttSetup| tools
homeassistant | homeassistant.cpp.o module_homeassistant.cpp.o | | | mqtt tools
domoticz | domoticz.cpp.o module_domoticz.cpp.o | | domoticzSendMagnitude | mqtt tools
thingspeak | thingspeak.cpp.o module_thingspeak.cpp.o | | tspkEnqueueMagnitude | http tools
light | light.cpp.o module_light.cpp.o | | lightBlue lightBrightness lightBrightnessPercent lightChannel lightChannels lightColdWhite lightColor lightGreen lightHasColdWhite lightHasColor lightHasWarmWhite lightHasWhite lightHs lightHsv lightMiredsRange lightOnReport lightRed lightRgb lightState lightTemperature lightTransitionStep lightTransitionTime lightUpdate lightUseRGB lightWarmWhite lightBrightnessStep lightMakeStateRelayProvider| tools
light_pwm | module_light_pwm.cpp.o | pwm.cpp.o | | light
light_my92xx | module_light_my92xx.cpp.o | | | light arduino
rf | rfbridge.cpp.o module_rf.cpp.o | | rfbSend rfbOnCode| tools arduino
matter | matter.cpp.o matter_app.cpp.o matter_identity.cpp.o module_matter.cpp.o | | matterWindowOpen matterWindowStart matterPairingCodes | mdns tools
matter_ble | matter_ble.cpp.o module_matter_ble.cpp.o | || ble matter tools
nspanel | nspanel.cpp.o nspanel_config.cpp.o nspanel_storage.cpp.o nspanel_net.cpp.o nspanel_link.cpp.o nspanel_flash.cpp.o nspanel_web.cpp.o nspanel_lovelace.cpp.o nspanel_ohpanel.cpp.o nspanelqr.cpp.o module_nspanel.cpp.o | | | uart tls http tools | 32768
uart | uart.cpp.o module_uart.cpp.o | | uartPort uartClaim uartClaimed uartOnClaim uartOpen uartBaud| tools
tls | module_tls.cpp.o | |
http | http.cpp.o module_http.cpp.o | || tools
captive | captive.cpp.o module_captive.cpp.o | || tools
spi | spi.cpp.o module_spi.cpp.o | || tools arduino
mcp23s08 | mcp23s08.cpp.o module_mcp23s08.cpp.o | | | spi tools arduino
rpnrules | rpnrules.cpp.o module_rpnrules.cpp.o | | | relay ntp tools arduino
encoder | encoder.cpp.o module_encoder.cpp.o | | | light tools
tuya | tuya.cpp.o module_tuya.cpp.o | | tuya::makeRelayProvider | relay light uart tools
uartmqtt | uartmqtt.cpp.o module_uartmqtt.cpp.o | | | uart mqtt tools
ir | ir.cpp.o module_ir.cpp.o | || tools arduino
rfm69 | rfm69.cpp.o module_rfm69.cpp.o | | | spi tools arduino
garland | garland.cpp.o module_garland.cpp.o | || tools arduino
thermostat | thermostat.cpp.o module_thermostat.cpp.o | | displayOn | relay sensors ntp tools arduino
influxdb | influxdb.cpp.o module_influxdb.cpp.o | | | http tools
prometheus | prometheus.cpp.o module_prometheus.cpp.o | | | api sensors tools
curtain_kingart | curtain_kingart.cpp.o module_curtain_kingart.cpp.o | | | uart ntp tools
ifan | ifan.cpp.o module_ifan.cpp.o | | fanMakeRelayProvider fanSpeed | relay tools
lightfox | lightfox.cpp.o module_lightfox.cpp.o | | lightfoxMakeRelayProvider lightfoxMakeButtonPin | relay button uart tools
ssdp | ssdp.cpp.o module_ssdp.cpp.o | |
llmnr | llmnr.cpp.o module_llmnr.cpp.o | |
netbios | netbios.cpp.o module_netbios.cpp.o | |
telnet_reverse | module_telnet_reverse.cpp.o | |
test | module_test.cpp.o | |

ESPURNA_MODULES */

// Modules on out of the box (names, space separated): the initial image of every chip carries them as a modules file
// at the start of the module area, the first boot installs them by the Apply; everything else comes by the Apply from
// the .ota. Read by scripts/module_initial_image.py (a regex on this line)
#define MODULES_ON_BY_DEFAULT       "captive relay button led"

#ifndef UART_MODULE
#define UART_MODULE                 0
#endif

#ifndef MODULE_BUILD
#define MODULE_BUILD                0
#endif

#if MODULE_BUILD

// --- the module objects: the module's part on, the loader and the forwarders are the head's

#undef MODULES_SUPPORT
#define MODULES_SUPPORT             0

#undef MQTT_MODULE
#define MQTT_MODULE                 0

#undef UART_MODULE
#define UART_MODULE                 0
#define UART_SUPPORT                1

#define MQTT_SUPPORT                1
#define RELAY_SUPPORT               1
#define BUTTON_SUPPORT              1
#define LED_SUPPORT                 1
#define API_SUPPORT                 1
#define NTP_SUPPORT                 1
#define MDNS_SERVER_SUPPORT         1
#define SCHEDULER_SUPPORT           1
#define HOMEASSISTANT_SUPPORT       1
#define ALEXA_SUPPORT               1
#define DOMOTICZ_SUPPORT            1
#define THINGSPEAK_SUPPORT          1

// the same modules on every chip: the sensors manager and every driver, light with every provider (PWM: the chip's driver,
// general.h PWM_PROVIDER), RF (the chip's provider, general.h RFB_PROVIDER)
// every sensor driver, a module each (the ESPURNA_MODULES list above)
#define A02YYU_SUPPORT              1
#define ADE7953_SUPPORT             1
#define AM2320_SUPPORT              1
#define ANALOG_SUPPORT              1
#define BH1750_SUPPORT              1
#define BME680_SUPPORT              1
#define BMP180_SUPPORT              1
#define BMX280_SUPPORT              1
#define CSE7766_SUPPORT             1
#define DALLAS_SUPPORT              1
#define DHT_SUPPORT                 1
#define DIGITAL_SUPPORT             1
#define DUMMY_SENSOR_SUPPORT        1
#define ECH1560_SUPPORT             1
#define EMON_ADC121_SUPPORT         1
#define EMON_ADS1X15_SUPPORT        1
#define EMON_ANALOG_SUPPORT         1
#define EVENTS_SUPPORT              1
#define EZOPH_SUPPORT               1
#define GEIGER_SUPPORT              1
#define GUVAS12SD_SUPPORT           1
#define HDC1080_SUPPORT             1
#define HLW8012_SUPPORT             1
#define INA219_SUPPORT              1
#define LDR_SUPPORT                 1
#define MAX6675_SUPPORT             1
#define MHZ19_SUPPORT               1
#define MICS2710_SUPPORT            1
#define MICS5525_SUPPORT            1
#define NTC_SUPPORT                 1
#define PM1006_SUPPORT              1
#define PMSX003_SUPPORT             1
#define PULSEMETER_SUPPORT          1
#define PZEM004T_SUPPORT            1
#define PZEM004TV30_SUPPORT         1
#define SDS011_SUPPORT              1
#define SENSEAIR_SUPPORT            1
#define SHT3X_I2C_SUPPORT           1
#define SI1145_SUPPORT              1
#define SI7021_SUPPORT              1
#define SM300D2_SUPPORT             1
#define SONAR_SUPPORT               1
#define T6613_SUPPORT               1
#define TMP3X_SUPPORT               1
#define V9261F_SUPPORT              1
#define VEML6075_SUPPORT            1
#define VL53L1X_SUPPORT             1
// the light module drives what its output modules register (light_pwm, light_my92xx, ...: lightSetProvider)
#define LIGHT_PROVIDER              LIGHT_PROVIDER_CUSTOM
#define PWM_SUPPORT                 1
#define RFB_SUPPORT                 1

// the functions of their own, a module each (owner 2026-11-12: «модулями, каждая свой модуль»)
#define SPI_SUPPORT                 1
#define MCP23S08_SUPPORT            1
#define RPN_RULES_SUPPORT           1
#define ENCODER_SUPPORT             1
#define TUYA_SUPPORT                1
#define UART_MQTT_SUPPORT           1
#define IR_SUPPORT                  1
#define RFM69_SUPPORT               1
#define GARLAND_SUPPORT             1
#define THERMOSTAT_SUPPORT          1
#define THERMOSTAT_DISPLAY_SUPPORT  1
#define INFLUXDB_SUPPORT            1
#define PROMETHEUS_SUPPORT          1
#define KINGART_CURTAIN_SUPPORT     1
#define IFAN_SUPPORT                1
#define LIGHTFOX_SUPPORT            1
#define SSDP_SUPPORT                1
#define LLMNR_SUPPORT               1
#define NETBIOS_SUPPORT             1

// what exists only on some chips: Matter (ESP32 and Beken, there is no Matter SDK for the ESP8266, the chip's ini
// leaves the module out: custom_modules_skip); the NSPanel (its display on a UART port) on every chip
#if defined(ESP32) || defined(BK72XX)
#define MATTER_SUPPORT              1
#endif
#define NSPANEL_SUPPORT             1

#elif defined(MODULES_SUPPORT) && MODULES_SUPPORT

// --- the head

#define WEB_SUPPORT                 1
#define TERMINAL_SUPPORT            1
#define TELNET_SUPPORT              1

// modules (owner 2026-11-12): the relays, buttons, LEDs (on by default in the initial image), the HTTP API, NTP; the
// head calls NTP through modules_api.cpp (the time of the log and the pages). The unstable mode switches every module
// off, these too (the old images did not switch either while the board kept crashing)
#define RELAY_SUPPORT               0
#define BUTTON_SUPPORT              0
#define LED_SUPPORT                 0
#define API_SUPPORT                 0
#define NTP_SUPPORT                 0

// MQTT: forwarders only, the client is the mqtt module
#define MQTT_SUPPORT                1
#define MQTT_MODULE                 1

// UART: forwarders only, the ports are the uart module (the serial log, the relays of a UART board and the modules
// that need it take the port when it starts)
#define UART_SUPPORT                1
#undef UART_MODULE
#define UART_MODULE                 1

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
