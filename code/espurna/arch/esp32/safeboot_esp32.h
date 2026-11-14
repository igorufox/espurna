/*

SAFEBOOT (ESP32)

Asymmetric flash layout (esp32_4mb_safeboot.csv, Tasmota's offsets): a factory app with the ESPurna safeboot image
(code/safeboot-app, reads Wi-Fi and the admin password from the ESPurna settings) and a single big OTA app for ESPurna:
- `safeboot` terminal command: next boot from the factory partition, where the new image is uploaded
- crash-loop guard: a few crashes in a row switch to safeboot, so a broken image can still be replaced over Wi-Fi
- web /upgrade answers with an error when there is no second OTA slot (Update would overwrite the running image)
- web /safeboot: migration from the two-slot layouts over Wi-Fi, an update through safeboot (an upload there),
  install / replace the safeboot image
- NVS / NVS.ERASE terminal commands: what fills the 20 KB NVS partition

*/

#pragma once

// The crash-loop guard, first thing after rtcmem (needs only RTC and the reset reason): a few unstable boots in a row
// switch to safeboot, whatever crashes after it
void safebootGuard();

// the step of the boot reached last (libs/boot_trace.h), shown by safeboot after a rollback
void safebootTrace(const char* name);

// a new image confirmed for the bootloader (its rollback), before a restart ESPurna asked for itself
void safebootConfirm();

// Early, right after the basic modules (works in the 'unstable' system mode as well)
void safebootSetup();

// The /safeboot page (flash layout migration, update from an URL) and the /upgrade guard, after webSetup()
void safebootWebSetup();
