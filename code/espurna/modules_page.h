/*

MODULES: the head's own page, every chip (modules.cpp arch::page, `/` while there is no webui module, and
/modules.html). Small on purpose: what the board runs, the memory, and the install of the WebUI (html/src/install.mjs
installImage, the same steps): tick, Apply: the head of the install image .ota first when the board runs another head
build (/modules/off, the head to /upgrade; the ESP32 head is not replaced over the network), then /modules/off and /modules/apply with the ticked names and the modules file. The full WebUI is the webui data
module; this page installs it. On top, the setup form (head_setup.cpp, W71): the admin password (required while the
default one is on) and the Wi-Fi network, with a scan, saved with a restart.

The page itself: espurna/modules_page.html, gzip'ed by scripts/head_page.py into modules_page_gz.h of the build dir
(HeadPageGz, served with Content-Encoding: gzip)

*/

#pragma once

#include <modules_page_gz.h>
