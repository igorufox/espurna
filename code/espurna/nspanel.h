/*

NSPANEL MODULE

Sonoff NSPanel (gen1, ESP32) display backend for the joBr99 "NSPanel Lovelace UI" TFT,
plus the board specific bits for the Tasmota safeboot flash layout

*/

#pragma once

// Early, right after the basic modules (works in the 'unstable' system mode as well):
// display power, crash-loop guard and the `safeboot` terminal command
void nspanelBootSetup();

// Display backend, MQTT, web endpoints, weather and temperature
void nspanelSetup();
