#pragma once
// WiFi of the layer, for the third-party libraries of the modules (FauxmoESP): the part of the Arduino-ESP32 WiFi
// class they call, over the chip's SDK calls of ESPurna (arch/rtos/wifi_sdk.h). ESPurna itself calls those directly

#include <Arduino.h>
#include <wifi_sdk.h>
#include "WiFiUdp.h"


typedef enum {
    WL_NO_SHIELD = 255,
    WL_IDLE_STATUS = 0,
    WL_NO_SSID_AVAIL = 1,
    WL_SCAN_COMPLETED = 2,
    WL_CONNECTED = 3,
    WL_CONNECT_FAILED = 4,
    WL_CONNECTION_LOST = 5,
    WL_DISCONNECTED = 6,
} wl_status_t;

#define WIFI_OFF WIFI_MODE_NULL
#define WIFI_STA WIFI_MODE_STA
#define WIFI_AP WIFI_MODE_AP
#define WIFI_AP_STA WIFI_MODE_APSTA

class WiFiClass {
public:
    // Mode
    bool mode(wifi_mode_t mode);
    wifi_mode_t getMode();
    bool enableSTA(bool enable);
    bool enableAP(bool enable);

    // The SDK keeps nothing in flash and never reconnects by itself: the common code decides (both stay off)
    void persistent(bool) {
    }

    bool setAutoReconnect(bool) {
        return true;
    }

    bool setSleep(bool enabled);

    // Station
    wl_status_t begin(const char* ssid, const char* passphrase = nullptr, int32_t channel = 0,
            const uint8_t* bssid = nullptr, bool connect = true);
    bool disconnect(bool wifioff = false, bool eraseap = false);
    bool config(IPAddress ip, IPAddress gateway, IPAddress subnet,
            IPAddress dns1 = IPAddress(), IPAddress dns2 = IPAddress());
    bool setHostname(const char* hostname);
    const char* getHostname();
    wl_status_t status();
    bool isConnected();

    IPAddress localIP();
    IPAddress subnetMask();
    IPAddress gatewayIP();

    String macAddress();
    uint8_t* macAddress(uint8_t* mac);
    bool setMacAddress(const uint8_t* mac);

    String SSID();
    uint8_t* BSSID(uint8_t* out = nullptr);
    String BSSIDstr();
    int8_t RSSI();
    int32_t channel();

    bool enableIPv6(bool enable = true);
    static const char* disconnectReasonName(wifi_err_reason_t reason);

    // Scan
    int16_t scanNetworks(bool async = false, bool show_hidden = false, bool passive = false,
            uint32_t max_ms_per_chan = 300, uint8_t channel = 0);
    int16_t scanComplete();
    void scanDelete();
    String SSID(uint8_t index);
    uint8_t* BSSID(uint8_t index, uint8_t* out = nullptr);
    String BSSIDstr(uint8_t index);
    int32_t RSSI(uint8_t index);
    int32_t channel(uint8_t index);
    wifi_auth_mode_t encryptionType(uint8_t index);

    // Access point
    bool softAP(const char* ssid, const char* passphrase = nullptr, int channel = 1,
            int ssid_hidden = 0, int max_connection = 4);
    bool softAPConfig(IPAddress local_ip, IPAddress gateway, IPAddress subnet);
    bool softAPdisconnect(bool wifioff = false);
    IPAddress softAPIP();
    String softAPSSID();
    uint8_t* softAPmacAddress(uint8_t* mac);
    String softAPmacAddress();
    uint8_t softAPgetStationNum();
};

extern WiFiClass WiFi;
