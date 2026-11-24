//------------------------------------------------------------------------------
// Do not change this file unless you know what you are doing
// Configuration settings are in the general.h file
//------------------------------------------------------------------------------

#pragma once

//------------------------------------------------------------------------------
// Various modules configuration

#if DEBUG_TELNET_SUPPORT
#undef TELNET_SUPPORT
#define TELNET_SUPPORT              1
#endif

#if not WEB_SUPPORT
#undef DEBUG_WEB_SUPPORT
#define DEBUG_WEB_SUPPORT           0
#endif

#if not WEB_SUPPORT
#undef API_SUPPORT
#define API_SUPPORT                 0           // API support requires web support
#endif

#if not WEB_SUPPORT
#undef SSDP_SUPPORT
#define SSDP_SUPPORT                0           // SSDP support requires web support
#endif

// a build for one board; generic images (the modules) claim the port of the log / terminal at runtime instead
#if UART_MQTT_SUPPORT && !UART_DRIVERS_CLAIM_PORT
#undef MQTT_SUPPORT
#define MQTT_SUPPORT                1           // UART<->MQTT requires MQTT and no serial debug & terminal
#undef UART_SUPPORT
#define UART_SUPPORT                1
#undef DEBUG_SERIAL_SUPPORT
#define DEBUG_SERIAL_SUPPORT        0
#undef TERMINAL_SERIAL_SUPPORT
#define TERMINAL_SERIAL_SUPPORT     0
#endif

#if (RELAY_PROVIDER_STM_SUPPORT || RELAY_PROVIDER_DUAL_SUPPORT) && UART_DRIVERS_CLAIM_PORT
#undef UART_SUPPORT
#define UART_SUPPORT                1           // generic images, the provider claims the port at runtime
#elif RELAY_PROVIDER_STM_SUPPORT || RELAY_PROVIDER_DUAL_SUPPORT
#undef UART_SUPPORT
#define UART_SUPPORT                1
#undef DEBUG_SERIAL_SUPPORT
#define DEBUG_SERIAL_SUPPORT        0
#undef TERMINAL_SERIAL_SUPPORT
#define TERMINAL_SERIAL_SUPPORT     0
#endif

#if not UART_SUPPORT
#undef DEBUG_SERIAL_SUPPORT
#define DEBUG_SERIAL_SUPPORT        0
#undef TERMINAL_SERIAL_SUPPORT
#define TERMINAL_SERIAL_SUPPORT     0
#endif

#if ALEXA_SUPPORT
#undef RELAY_SUPPORT
#define RELAY_SUPPORT               1               // alexa needs some switches support to work
#undef WEB_SUPPORT
#define WEB_SUPPORT                 1               // fauxmoESP answers through the web server (no TCP server of its own)
#endif

#if RPN_RULES_SUPPORT
#undef MQTT_SUPPORT
#define MQTT_SUPPORT                1
#endif

#if DOMOTICZ_SUPPORT
#undef MQTT_SUPPORT
#define MQTT_SUPPORT                1               // If Domoticz enabled enable MQTT
#endif

#if HOMEASSISTANT_SUPPORT
#undef MQTT_SUPPORT
#define MQTT_SUPPORT                1               // If Home Assistant enabled enable MQTT
#endif

#if THERMOSTAT_SUPPORT
#undef MQTT_USE_JSON
#define MQTT_USE_JSON               1           // Thermostat depends on group messages in a JSON body
#undef RELAY_SUPPORT
#define RELAY_SUPPORT               1           // Thermostat depends on switches
#endif

#if SCHEDULER_SUPPORT
#undef NTP_SUPPORT
#define NTP_SUPPORT                 1           // Scheduler needs NTP to work
#undef RELAY_SUPPORT
#define RELAY_SUPPORT               1           // Scheduler needs relays
#endif

#if TUYA_SUPPORT && !UART_DRIVERS_CLAIM_PORT    // generic images: ltTime / ltStep / useTransitions of the board
#undef LIGHT_TRANSITION_TIME
#define LIGHT_TRANSITION_TIME       1600       // longer transition than the default
#undef LIGHT_TRANSITION_STEP
#define LIGHT_TRANSITION_STEP       200        // step can't be 10ms since most tuya serial connections are not fast
#undef LIGHT_USE_TRANSITIONS
#define LIGHT_USE_TRANSITIONS       0          // also, disable transitions unless set at runtime
#endif

#if TUYA_SUPPORT
#undef RELAY_SUPPORT
#define RELAY_SUPPORT               1           // Most of the time we require it
#endif


#if TERMINAL_WEB_API_SUPPORT
#undef TERMINAL_SUPPORT
#define TERMINAL_SUPPORT            1           // Need terminal command line parser and commands
#undef WEB_SUPPORT
#define WEB_SUPPORT                 1           // Registered as web server request handler
#endif

#if TERMINAL_MQTT_SUPPORT
#undef TERMINAL_SUPPORT
#define TERMINAL_SUPPORT            1           // Need terminal command line parser and commands
#undef MQTT_SUPPORT
#define MQTT_SUPPORT                1           // Subscribe and publish things
#endif

#if IFAN_SUPPORT
#undef RELAY_SUPPORT
#define RELAY_SUPPORT               1           // Need relays to manage general state
#endif

#if LIGHT_PROVIDER_DIMMER_SUPPORT
#undef PWM_SUPPORT
#define PWM_SUPPORT                 1           // Need PWM to update channel values
#endif



#if !DEBUG_SUPPORT
#undef DEBUG_LOG_BUFFER_SUPPORT
#define DEBUG_LOG_BUFFER_SUPPORT  0              // Can't buffer if there is no debugging enabled.
                                                 // Helps to avoid checking twice for both DEBUG_SUPPORT and BUFFER_LOG_SUPPORT
#endif

//------------------------------------------------------------------------------
// Prometheus needs web server + request handler API

#if PROMETHEUS_SUPPORT
#undef WEB_SUPPORT
#define WEB_SUPPORT 1
#endif

//------------------------------------------------------------------------------
// Analog pin needs ADC_TOUT mode set up at compile time

#if BUTTON_PROVIDER_ANALOG_SUPPORT
#undef ADC_MODE_VALUE
#define ADC_MODE_VALUE ADC_TOUT
#endif

//------------------------------------------------------------------------------
// RFBRIDGE EFM provider needs serial support (also when it is picked at runtime, RFB_PROVIDER_ANY)
#if RFB_SUPPORT && RFB_PROVIDER_EFM8BB1_SUPPORT
#undef UART_SUPPORT
#define UART_SUPPORT 1
#endif

//------------------------------------------------------------------------------
// Forcibly disable UART logger and terminal, these modules usually
// are expecting to work with the port exclusivelly
// (an so we could more easily describe things in hardware .h)
#if (\
    ((CSE7766_SUPPORT) && (CSE7766_PORT == DEBUG_SERIAL_PORT)) || \
    ((EZOPH_SUPPORT) && (EZOPH_PORT == DEBUG_SERIAL_PORT)) || \
    ((KINGART_CURTAIN_SUPPORT) && (KINGART_CURTAIN_PORT == DEBUG_SERIAL_PORT)) || \
    ((MHZ19_SUPPORT) && (MHZ19_PORT == DEBUG_SERIAL_PORT)) || \
    ((PM1006_SUPPORT) && (PM1006_PORT == DEBUG_SERIAL_PORT)) || \
    ((PMSX003_SUPPORT) && (PMSX003_PORT == DEBUG_SERIAL_PORT)) || \
    ((PZEM004TV30_SUPPORT) && (PZEM004TV30_PORT == DEBUG_SERIAL_PORT)) || \
    ((PZEM004T_SUPPORT) && (PZEM004T_PORT == DEBUG_SERIAL_PORT)) || \
    ((RELAY_PROVIDER_DUAL_SUPPORT) && !(UART_DRIVERS_CLAIM_PORT) && (RELAY_PROVIDER_DUAL_PORT == DEBUG_SERIAL_PORT)) || \
    ((RELAY_PROVIDER_STM_SUPPORT) && !(UART_DRIVERS_CLAIM_PORT) && (RELAY_PROVIDER_STM_PORT == DEBUG_SERIAL_PORT)) || \
    ((RFB_PROVIDER == RFB_PROVIDER_EFM8BB1) && (RFB_PORT == DEBUG_SERIAL_PORT)) || \
    ((SDS011_SUPPORT) && (SDS011_PORT == DEBUG_SERIAL_PORT)) || \
    ((SENSEAIR_SUPPORT) && (SENSEAIR_PORT == DEBUG_SERIAL_PORT)) || \
    ((SM300D2_SUPPORT) && (SM300D2_PORT == DEBUG_SERIAL_PORT)) || \
    ((T6613_SUPPORT) && (T6613_PORT == DEBUG_SERIAL_PORT)) || \
    ((TUYA_SUPPORT) && (TUYA_PORT == DEBUG_SERIAL_PORT)) || \
    ((V9261F_SUPPORT) && (V9261F_PORT == DEBUG_SERIAL_PORT)) || \
    ((LIGHTFOX_SUPPORT) && !(UART_DRIVERS_CLAIM_PORT) && (LIGHTFOX_PORT == DEBUG_SERIAL_PORT)) \
)
#undef DEBUG_SERIAL_SUPPORT
#define DEBUG_SERIAL_SUPPORT 0
#endif

#if (\
    ((CSE7766_SUPPORT) && (CSE7766_PORT == TERMINAL_SERIAL_PORT)) || \
    ((EZOPH_SUPPORT) && (EZOPH_PORT == TERMINAL_SERIAL_PORT)) || \
    ((KINGART_CURTAIN_SUPPORT) && (KINGART_CURTAIN_PORT == TERMINAL_SERIAL_PORT)) || \
    ((MHZ19_SUPPORT) && (MHZ19_PORT == TERMINAL_SERIAL_PORT)) || \
    ((PM1006_SUPPORT) && (PM1006_PORT == TERMINAL_SERIAL_PORT)) || \
    ((PMSX003_SUPPORT) && (PMSX003_PORT == TERMINAL_SERIAL_PORT)) || \
    ((PZEM004TV30_SUPPORT) && (PZEM004TV30_PORT == TERMINAL_SERIAL_PORT)) || \
    ((PZEM004T_SUPPORT) && (PZEM004T_PORT == TERMINAL_SERIAL_PORT)) || \
    ((RELAY_PROVIDER_DUAL_SUPPORT) && !(UART_DRIVERS_CLAIM_PORT) && (RELAY_PROVIDER_DUAL_PORT == TERMINAL_SERIAL_PORT)) || \
    ((RELAY_PROVIDER_STM_SUPPORT) && !(UART_DRIVERS_CLAIM_PORT) && (RELAY_PROVIDER_STM_PORT == TERMINAL_SERIAL_PORT)) || \
    ((RFB_PROVIDER == RFB_PROVIDER_EFM8BB1) && (RFB_PORT == TERMINAL_SERIAL_PORT)) || \
    ((SDS011_SUPPORT) && (SDS011_PORT == TERMINAL_SERIAL_PORT)) || \
    ((SENSEAIR_SUPPORT) && (SENSEAIR_PORT == TERMINAL_SERIAL_PORT)) || \
    ((SM300D2_SUPPORT) && (SM300D2_PORT == TERMINAL_SERIAL_PORT)) || \
    ((T6613_SUPPORT) && (T6613_PORT == TERMINAL_SERIAL_PORT)) || \
    ((TUYA_SUPPORT) && (TUYA_PORT == TERMINAL_SERIAL_PORT)) || \
    ((V9261F_SUPPORT) && (V9261F_PORT == TERMINAL_SERIAL_PORT)) || \
    ((LIGHTFOX_SUPPORT) && !(UART_DRIVERS_CLAIM_PORT) && (LIGHTFOX_PORT == TERMINAL_SERIAL_PORT)) \
)
#undef TERMINAL_SERIAL_SUPPORT
#define TERMINAL_SERIAL_SUPPORT 0
#endif

