/*

SSDP MODULE

Copyright (C) 2017-2019 by Xose Pérez <xose dot perez at gmail dot com>
Uses SSDP library by PawelDino (https://github.com/PawelDino)
https://github.com/esp8266/Arduino/issues/2283#issuecomment-299635604

*/

#include "espurna.h"

#if SSDP_SUPPORT

#include "libs/udp.h"

#include "web.h"

namespace ssdp {
namespace {
namespace settings {

espurna::string name() {
    return getSetting("ssdpName", systemHostname());
}

// needs to be in the response
// ref. https://github.com/esp8266/Arduino/issues/2283

espurna::string type() {
    return getSetting("ssdpType", SSDP_DEVICE_TYPE);
}

espurna::string udn() {
    espurna::string out;
    out += "38323636-4558-4dda-9188-cda0e6";
    out += espurna::string(systemShortChipId());
    return out;
}

} // namespace settings

espurna::string entry(const espurna::string& tag, const espurna::string& value) {
    espurna::string out;
    out.reserve((tag.length() * 2) + value.length() + 16);

    out += '<';
    out += tag;
    out += '>';

    out += value;

    out += "</";
    out += tag;
    out += '>';

    return out;
}

espurna::string entry(const espurna::string& tag, espurna::StringView value) {
    return entry(tag, espurna::string(value));
}

espurna::string response() {
    espurna::string out;
    out.reserve(512);

    out += "<?xml version=\"1.0\"?>"
             "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
             "<specVersion>"
                "<major>1</major>"
                "<minor>0</minor>"
             "</specVersion>";

    // <URLBase>http://%s:%u/</URLBase>
    espurna::string base;
    base += "http://";
    base += ipToString(wifiStaIp());
    base += ':';
    base += espurna::string(webPort(), 10);
    base += '/';

    out += entry("URLBase", base);

    // <device> ... </device>
    espurna::string device;

    // <deviceType>%s</deviceType>
    device += entry("deviceType", settings::type());

    // <friendlyName>%s</friendlyName>
    device += entry("friendlyName", settings::name());

    // <presentationURL>/</presentationURL>
    device += entry("presentationURL", espurna::string('/'));

    // <serialNumber>%u</serialNumber>
    device += entry("serialNumber", systemShortChipId());

    const auto app = buildApp();

    // <modelName>%s</modelName>
    device += entry("modelName", app.name);

    // <modelNumber>%s</modelNumber>
    device += entry("modelNumber", app.version);

    // <modelURL>%s</modelURL>
    device += entry("modelURL", app.website);

    // <manufacturer>%s</manufacturer>
    device += entry("manufacturer", systemDevice());

    // <manufacturerURL>%s</manufacturerURL>
    device += entry("manufacturerURL", app.website);

    // <UDN>uuid:38323636-4558-4dda-9188-cda0e6%06x</UDN>
    device += entry("UDN", settings::udn());

    out += entry("device", device);
    out += "</root>";

    return out;
}

// --- the responder: M-SEARCH on 239.255.255.250:1900 answered with where description.xml is (the same on every
// chip, on lwIP: libs/udp.h; the cores' SSDP libraries are gone, Beken never had one)

constexpr uint16_t Port { 1900 };

espurna::udp::Socket udp;

bool wanted(const espurna::string& st) {
    return st.equalsIgnoreCase("ssdp:all")
        || st.equalsIgnoreCase("upnp:rootdevice")
        || st.equalsIgnoreCase(settings::type())
        || st.equalsIgnoreCase(espurna::string("uuid:") + settings::udn());
}

// "ST: <value>" of a request, empty when there is none
espurna::string header(const espurna::string& request, const char* name) {
    const espurna::string key(name);
    int at = 0;
    while (at < static_cast<int>(request.length())) {
        int end = request.indexOf('\n', at);
        if (end < 0) {
            end = request.length();
        }

        espurna::string line = request.substring(at, end);
        line.trim();
        at = end + 1;

        const int colon = line.indexOf(':');
        if ((colon > 0) && line.substring(0, colon).equalsIgnoreCase(key)) {
            espurna::string value = line.substring(colon + 1);
            value.trim();
            return value;
        }
    }

    return espurna::string();
}

// `to` IPv4 in network order (libs/udp.h)
void answer(const espurna::string& st, uint32_t to, uint16_t port) {
    espurna::string out;
    out.reserve(320);
    out += "HTTP/1.1 200 OK\r\nEXT:\r\nCACHE-CONTROL: max-age=1800\r\nSERVER: ESPurna UPnP/1.1\r\nST: ";
    out += st;
    out += "\r\nUSN: uuid:";
    out += settings::udn();
    if (!st.startsWith("uuid:")) {
        out += "::";
        out += st;
    }
    out += "\r\nLOCATION: http://";
    out += ipToString(wifiStaIp());
    out += ':';
    out += espurna::string(webPort(), 10);
    out += "/description.xml\r\n\r\n";

    espurna::udp::Message message(out.length());
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
    udp.onPacket([](espurna::udp::Packet& packet) {
        espurna::string request;
        request.concat(reinterpret_cast<const char*>(packet.data()), packet.length());
        if (!request.startsWith("M-SEARCH")) {
            return;
        }

        const auto st = header(request, "ST");
        if (wanted(st)) {
            answer(st.equalsIgnoreCase("ssdp:all") ? espurna::string("upnp:rootdevice") : st,
                packet.remoteIP(), packet.remotePort());
        }
    });

    if (!udp.listen(Port) || !udp.joinGroup(espurna::udp::ip4(239, 255, 255, 250))) {
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

