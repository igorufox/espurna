# coding=utf-8
# pylint: dummy-variables-rgx='(_+[a-zA-Z0-9]*?$)|dummy|env|projenv'
#
# Original extra_scripts.py
# Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>
#
# ldscripts, lwip patching, updated postmortem flags and git support
# Copyright (C) 2019-2020 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

# The post: script of every chip (platformio_esp8266.ini, platformio_esp32.ini, platformio_beken.ini), run AFTER the
# platform code is loaded:
# - every chip: APP_VERSION with the git revision (26.11.0-git<sha>, the WebUI release list finds the release the board
#   runs by it), the ESPURNA_BUILD_* / ESPURNA_BOARD / ESPURNA_FLAGS overrides of the OS environment, warnings of the
#   project code, the compile_commands.json defines, the CacheDir fix
# (the ESP8266 part of the NONOS build - its map, ldscripts, elf2bin, postmortem and float switches - went with NONOS,
# 2026-11-17: every chip is on its SDK's own link and image tools)

import os
import itertools
import sys

from SCons.Script import Import

Import("env", "projenv")
env = globals()["env"]
projenv = globals()["projenv"]

# espurna_utils is in this folder: on sys.path while SCons runs this script, not always for the scripts after it
sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "scripts"))

from espurna_utils import (  # noqa: E402
    app_add_compiledb_defines,
    app_inject_flags,
    app_inject_version,
    app_patch_cachedir,
    check_env,
)

# the parts of the version string, the name and destination of `-t build-and-copy`, the board and flags of ota.py:
# from the OS environment (build.sh, ota.py, CI)
for name in (
    "ESPURNA_BUILD_NAME",
    "ESPURNA_BUILD_DESTINATION",
    "ESPURNA_BUILD_FULL_VERSION",
    "ESPURNA_BUILD_VERSION",
    "ESPURNA_BUILD_REVISION",
    "ESPURNA_BUILD_VERSION_SUFFIX",
    "ESPURNA_BOARD",
    "ESPURNA_FLAGS",
):
    value = os.environ.get(name)
    if value:
        env.Replace(**{name: value})
        projenv.Replace(**{name: value})

# Always show warnings for project code
projenv.ProcessUnFlags("-w")

# override static version flag from the espurna/config/version.h
# either completely, or change the version / revision / suffix part separately
app_inject_version(projenv)

# ESPURNA_BOARD and ESPURNA_FLAGS (only -D...), here since projenv is not available in pre-scripts
app_inject_flags(projenv)

# pre-processing single-source only makes sense from projenv
# (however, this means dependencies are already attached)
single_source = env.get("ESPURNA_SINGLE_SOURCE_TARGET")
if single_source:
    projenv.AddCustomTarget(
        "dependencies",
        [
            projenv.PreProcess(single_source),
            projenv.SourceDep(single_source),
        ],
        None,
        title="Intermediate dependency files",
        description=None,
        always_build=False,
    )

# hijack SCons signature generation to exclude BUILD_DIR, making our builds faster
# when using different environments (i.e. itead-sonoff-basic, itead-sonoff-pow, etc.)
if check_env("ESPURNA_FIX_CACHEDIR_PATH", "n"):
    app_patch_cachedir(projenv)

# modules are implicitly enabled when generating compile_commands.json
# disabled by default, to avoid silently hijacking config values from headers
if check_env("ESPURNA_COMPILEDB_DEFINES", "0"):
    app_add_compiledb_defines(projenv)
