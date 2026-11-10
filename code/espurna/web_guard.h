/*

WEB GUARD

New TCP connections to the web server and telnet wait while the heap is low, the same on every chip (web_guard.cpp)

*/

#pragma once

#include <cstdint>

// connections reset by the guard since boot
uint32_t webGuardRefused();

// after the web server started: puts the gate on the listeners (web, telnet), settings webGuard*
void webGuardSetup();
