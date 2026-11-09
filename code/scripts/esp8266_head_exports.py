# PROTOTYPE (docs/esp8266-modules.md): head functions that modules call although the head itself does not, kept in
# the head link (-Wl,-u). The list (custom_head_exports, one mangled name per line) is written by
# scripts/esp8266_module_link.py when a module link finds a head function missing; rebuild the head after that.

import os

from SCons.Script import Import

Import("env")
env = globals()["env"]

path = env.GetProjectOption("custom_head_exports", "")
if path:
    path = os.path.join(env.subst("$PROJECT_DIR"), path)
    names = []
    if os.path.exists(path):
        with open(path) as f:
            names = [line.strip() for line in f if line.strip() and not line.startswith("#")]
    # one implicit linker script with EXTERN(): -Wl,-u for each made the command line too long for Windows (ld 5)
    script = os.path.join(env.subst("$BUILD_DIR"), "head-exports.ld")
    os.makedirs(os.path.dirname(script), exist_ok=True)
    with open(script, "w", newline=chr(10)) as f:
        for name in names:
            f.write(f"EXTERN({name})" + chr(10))
    env.Append(LINKFLAGS=[script.replace(chr(92), "/")])
    # the list changes without the command line changing: relink then
    env.Depends("$BUILD_DIR/${PROGNAME}.elf", path)
    print(f"Head exports: {len(names)} from {path}")

# ArduinoOTA references mDNS although ESPurna starts it without (begin(false)): the head takes stubs instead
# (modules_api_esp8266.cpp), the real mDNS (~21 KB) is the mdns module's
HEAD_WRAPS = [
    "MDNS",
    "_ZN7esp826618MDNSImplementation13MDNSResponder13enableArduinoEtb",
    "_ZN7esp826618MDNSImplementation13MDNSResponder3endEv",
    "_ZN7esp826618MDNSImplementation13MDNSResponder5beginEPKcRK9IPAddressj",
    "_ZN7esp826618MDNSImplementation13MDNSResponder6updateEv",
]
env.Append(LINKFLAGS=[f"-Wl,--wrap={name}" for name in HEAD_WRAPS if name != "MDNS"])
