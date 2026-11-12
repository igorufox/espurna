/*

SAFEBOOT (ESP32)

Asymmetric flash layout (esp32_4mb_safeboot.csv, Tasmota's offsets): a factory app with the ESPurna safeboot image
(code/safeboot-app, reads Wi-Fi and the admin password from the ESPurna settings) and a single big OTA app for ESPurna:
- `safeboot` terminal command: next boot from the factory partition, where the new image is uploaded
- crash-loop guard: a few crashes in a row switch to safeboot, so a broken image can still be replaced over Wi-Fi
- web /upgrade answers with an error when there is no second OTA slot (Update would overwrite the running image)
- web /safeboot: migration from the two-slot layouts over Wi-Fi, update from an URL or a file through safeboot,
  install / replace the safeboot image
- NVS / NVS.ERASE terminal commands: what fills the 20 KB NVS partition

*/

#pragma once

// Early, right after the basic modules (works in the 'unstable' system mode as well)
void safebootSetup();

// The URL (and the SHA-256, may be empty) for safeboot, the next boot goes there (ota_fetch.cpp). The caller restarts
bool safebootUpdateFromUrl(const String& url, const String& sha256, String& error);

// What safeboot did with the last URL ("downloaded ...", "failed: ..."): NVS "safeboot"/"result", which safeboot
// writes. The one result of an update by an URL on the ESP32 (/ota/status, /safeboot/status, `ota`)
String safebootResult();

// the URL safeboot was given last (NVS "safeboot"/"url"), "" when none
String safebootUrl();

// The /safeboot page (flash layout migration, update from an URL) and the /upgrade guard, after webSetup()
void safebootWebSetup();
