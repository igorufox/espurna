# GCC 14 on the Beken (U25, libraries.ini beken_toolchain): GCC 14 turned three old C warnings into errors by default
# (implicit function declarations, incompatible pointer types, int <-> pointer conversions), and the C sources of the
# BDK / LibreTiny core have them (base/fixups/sys_ctrl.c, wlan_ui.c: ps_delay, get_ate_mode_state, ip_address_set ...).
# Back to warnings, as GCC 10.3 had them; C only, the C++ of ESPurna is not touched.
#
# At the end of the C command line (CCCOM), not in CFLAGS: LibreTiny adds -Werror=implicit-function-declaration to the
# CFLAGS of its library queue after this script (builder/frameworks/base.py), the last option wins. The queue clones
# this env, so its libraries (the core, the BDK) get the command as well
#
# pylint: disable=undefined-variable

Import("env")

env.Replace(ESPURNA_GCC14_C=[
    "-Wno-error=implicit-function-declaration",
    "-Wno-error=incompatible-pointer-types",
    "-Wno-error=int-conversion",
])
env.Replace(CCCOM=env["CCCOM"] + " $ESPURNA_GCC14_C")
