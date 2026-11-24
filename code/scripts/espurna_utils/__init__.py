# coding=utf-8
#
# Original extra_scripts.py
# Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>
#
# ldscripts, lwip patching, updated postmortem flags and git support
# Copyright (C) 2019-2021 by Maxim Prokhorov <prokhorov dot max at outlook dot com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

from .build import (
    firmware_destination,
    app_add_builder_single_source,
    app_add_target_build_and_copy,
    app_add_gzip_file,
    app_add_target_build_re2c,
    app_patch_cachedir,
)
from .checks import check_env
from .config import app_add_compiledb_defines
from .flags import app_inject_flags
from .version import app_inject_version, app_full_version_for_env

__all__ = [
    "app_add_builder_single_source",
    "app_add_compiledb_defines",
    "app_add_gzip_file",
    "app_add_target_build_and_copy",
    "app_add_target_build_re2c",
    "app_full_version_for_env",
    "app_inject_flags",
    "app_inject_version",
    "app_patch_cachedir",
    "app_version",
    "check_env",
    "firmware_destination",
]
