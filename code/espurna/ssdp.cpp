/*

SSDP MODULE

Copyright (C) 2017-2019 by Xose Pérez <xose dot perez at gmail dot com>
Uses SSDP library by PawelDino (https://github.com/PawelDino)
https://github.com/esp8266/Arduino/issues/2283#issuecomment-299635604

*/

#include "espurna.h"

#if SSDP_SUPPORT

#include <EspurnaUdp.h>

#include "web.h"

namespace ssdp {
namespace {
namespace settings {

String name() {
    return getSetting("ssdpName", systemHostname());
}

// needs to be in the response
// ref. https://github.com/esp8266/Arduino/issues/2283

String type() {
    return getSetting("ssdpType", F(SSDP_DEVICE_TYPE));
}

String udn() {
    String out;
    out += F("38323636-4558-4dda-9188-cda0e6");
    out += String(systemShortChipId());
    return out;
}

} // namespace settings

String entry(const String& tag, const String& value) {
    String out;
    out.reserve((tag.length() * 2) + value.length() + 16);

    out += '<';
    out += tag;
    out += '>';

    out += value;

    out += F("</");
    out += tag;
    out += '>';

    return out;
}

String entry(const String& tag, espurna::StringView value) {
    return entry(tag, String(value));
}

String response() {
    String out;
    out.reserve(512);

    out += F("<?xml version=\"1.0\"?>"
             "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
             "<specVersion>"
                "<major>1</major>"
                "<minor>0</minor>"
             "</specVersion>");

    // <URLBase>http://%s:%u/</URLBase>
    String base;
    base += F("http://");
    base += ipToString(WiFi.localIP());
    base += ':';
    base += String(webPort(), 10);
    base += '/';

    out += entry(F("URLBase"), base);

    // <device> ... </device>
    String device;

    // <deviceType>%s</deviceType>
    device += entry(F("deviceType"), settings::type());

    // <friendlyName>%s</friendlyName>
    device += entry(F("friendlyName"), settings::name());

    // <presentationURL>/</presentationURL>
    device += entry(F("presentationURL"), String('/'));

    // <serialNumber>%u</serialNumber>
    device += entry(F("serialNumber"), systemShortChipId());

    const auto app = buildApp();

    // <modelName>%s</modelName>
    device += entry(F("modelName"), app.name);

    // <modelNumber>%s</modelNumber>
    device += entry(F("modelNumber"), app.version);

    // <modelURL>%s</modelURL>
    device += entry(F("modelURL"), app.website);

    // <manufacturer>%s</manufacturer>
    device += entry(F("manufacturer"), systemDevice());

    // <manufacturerURL>%s</manufacturerURL>
    device += entry(F("manufacturerURL"), app.website);

    // <UDN>uuid:38323636-4558-4dda-9188-cda0e6%06x</UDN>
    device += entry(F("UDN"), settings::udn());

    out += entry(F("device"), device);
    out += F("</root>");

    return out;
}

// --- the responder: M-SEARCH on 239.255.255.250:1900 answered with where description.xml is (the same on every
// chip, on lwIP: EspurnaUdp of lib/DNSServer; the cores' SSDP libraries are gone, Beken never had one)

constexpr uint16_t Port { 1900 };

EspurnaUdp udp;

bool wanted(const String& st) {
    return st.equalsIgnoreCase(F("ssdp:all"))
        || st.equalsIgnoreCase(F("upnp:rootdevice"))
        || st.equalsIgnoreCase(settings::type())
        || st.equalsIgnoreCase(String(F("uuid:")) + settings::udn());
}

// "ST: <value>" of a request, empty when there is none
String header(const String& request, const __FlashStringHelper* name) {
    const String key(name);
    int at = 0;
    while (at < static_cast<int>(request.length())) {
        int end = request.indexOf('\n', at);
        if (end < 0) {
            end = request.length();
        }

        String line = request.substring(at, end);
        line.trim();
        at = end + 1;

        const int colon = line.indexOf(':');
        if ((colon > 0) && line.substring(0, colon).equalsIgnoreCase(key)) {
            String value = line.substring(colon + 1);
            value.trim();
            return value;
        }
    }

    return String();
}

void answer(const String& st, const IPAddress& to, uint16_t port) {
    String out;
    out.reserve(320);
    out += F("HTTP/1.1 200 OK\r\nEXT:\r\nCACHE-CONTROL: max-age=1800\r\nSERVER: ESPurna UPnP/1.1\r\nST: ");
    out += st;
    out += F("\r\nUSN: uuid:");
    out += settings::udn();
    if (!st.startsWith(F("uuid:"))) {
        out += F("::");
        out += st;
    }
    out += F("\r\nLOCATION: http://");
    out += ipToString(WiFi.localIP());
    out += ':';
    out += String(webPort(), 10);
    out += F("/description.xml\r\n\r\n");

    EspurnaUdpMessage message(out.length());
    if (message) {
        message.write(reinterpret_cast<const uint8_t*>(out.c_str()), out.length());
        udp.sendTo(message, to, port);
    }
}

void setup() {
    webServer().on("/description.xml", WebMethod::Get, [](WebRequest& request) {
        webSend(request, 200, "text/xml", response());
    });

    // lwIP's context: answered right there (a short text, no loop needed)
    udp.onPacket([](EspurnaUdpPacket& packet) {
        String request;
        request.concat(reinterpret_cast<const char*>(packet.data()), packet.length());
        if (!request.startsWith(F("M-SEARCH"))) {
            return;
        }

        const auto st = header(request, F("ST"));
        if (wanted(st)) {
            answer(st.equalsIgnoreCase(F("ssdp:all")) ? String(F("upnp:rootdevice")) : st,
                packet.remoteIP(), packet.remotePort());
        }
    });

    if (!udp.listen(Port) || !udp.joinGroup(IPAddress(239, 255, 255, 250))) {
        DEBUG_MSG_P(PSTR("[SSDP] Cannot listen on %u\n"), Port);
        return;
    }

    DEBUG_MSG_P(PSTR("[SSDP] Answering as %s\n"), settings::name().c_str());
}

} // namespace
} // namespace ssdp

void ssdpSetup() {
    ssdp::setup();
}

#endif // SSDP_SUPPORT

