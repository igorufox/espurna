#!/usr/bin/env python3
"""Lint of the PlatformIO configs of the CI (.github/workflows/esp32.yml, first job).

    python scripts/ci_ini_lint.py platformio_esp8266.ini platformio_esp32.ini platformio_beken.ini

Run from code/ (extra_configs are relative to it, same as for `pio run -c ...`). Fails on:
- `-U<NAME>` in the build_flags / build_src_flags of an env: PlatformIO puts every -U after every -D, so
  `-UX -DX=1` leaves X undefined (06.11: options silently off)
- a macro defined twice with different values in one env's resolved build_flags + build_src_flags
  (`-DX` counts as `-DX=1`)
- a build dir other than .pio/build_esp8266, .pio/build_esp32, .pio/build_beken (one per chip): `build_dir` of
  [platformio] missing (PlatformIO's default .pio/build) or anything else, and `build_dir` in any other section
- envs: exactly one per chip, named after it (platformio_<chip>.ini has env:<chip> and nothing else, its extra
  configs included): the head, every module and the release images are one build
- custom_module_flags (the macros of the module objects, scripts/module_link.py): -U or two values for one macro

Values are resolved with PlatformIO's ProjectConfig when it can be imported (same resolution as the build: extends,
[env], ${section.option}, extra_configs), otherwise with configparser and the same rules re-implemented here.
"""

import argparse
import configparser
import glob
import os
import re
import shlex
import sys

ALLOWED_BUILD_DIRS = (".pio/build_esp8266", ".pio/build_esp32", ".pio/build_beken")
DEFAULT_BUILD_DIR = ".pio/build"
FLAG_OPTIONS = ("build_flags", "build_src_flags")
MODULE_FLAG_OPTIONS = ("custom_module_flags",)
CHIP_RE = re.compile(r"platformio_([a-z0-9]+)\.ini$")


# ---------------------------------------------------------------------------
# fallback resolver, ref. platformio/project/config.py (6.x): walk_options(), _re_interpolation_handler()
# ---------------------------------------------------------------------------

VARTPL_RE = re.compile(r"\$\{(?:([^\.\}\()]+)\.)?([^\}]+)\}")
INLINE_COMMENT_RE = re.compile(r"\s+;.*$")


def parse_multi_values(items):
    result = []
    if not items:
        return result
    if not isinstance(items, (list, tuple)):
        items = items.split("\n" if "\n" in items else ", ")
    for item in items:
        item = item.strip()
        if not item or item.startswith((";", "#")):
            continue
        if ";" in item:
            item = INLINE_COMMENT_RE.sub("", item).strip()
        result.append(item)
    return result


class FallbackConfig:
    """Just enough of ProjectConfig: envs(), get(section, option)"""

    MULTIPLE = {"build_flags", "build_src_flags", "extends", "extra_configs"}

    def __init__(self, path):
        self.parser = configparser.ConfigParser(inline_comment_prefixes=("#", ";"), interpolation=None)
        self.parsed = []
        self.read(path)

    def read(self, path):
        if path in self.parsed:
            return
        self.parsed.append(path)
        with open(path, encoding="utf-8") as f:
            self.parser.read_file(f, path)
        if self.parser.has_option("platformio", "extra_configs"):
            for pattern in parse_multi_values(self.parser.get("platformio", "extra_configs")):
                for item in sorted(glob.glob(os.path.expanduser(pattern))):
                    self.read(item)

    def envs(self):
        return [section[4:] for section in self.parser.sections() if section.startswith("env:")]

    def walk_options(self, root_section):
        queue = ["env", root_section] if root_section.startswith("env:") else [root_section]
        while queue:
            section = queue.pop()
            if not self.parser.has_section(section):
                continue
            for option in self.parser.options(section):
                yield section, option
            if self.parser.has_option(section, "extends"):
                queue.extend(parse_multi_values(self.parser.get(section, "extends")))

    def getraw(self, section, option):
        for found_section, found_option in self.walk_options(section):
            if found_option == option:
                return self.parser.get(found_section, option)
        return None

    def get(self, section, option, default=None, depth=0):
        if depth > 50:
            raise ValueError(f"interpolation loop at [{section}] {option}")
        value = self.getraw(section, option)
        if value is None:
            return default

        def handler(match):
            ref_section, ref_option = match.group(1), match.group(2)
            if ref_section is None:
                return match.group(0)  # SCons variable / builtin, left as is
            if ref_section == "sysenv":
                return os.getenv(ref_option, "")
            if ref_section == "this":
                if ref_option == "__env__":
                    return section[4:]
                ref_section = section
            ref = self.get(ref_section, ref_option, "", depth + 1)
            return "\n".join(ref) if isinstance(ref, list) else str(ref)

        if "${" in value and "}" in value:
            value = VARTPL_RE.sub(handler, value)
        if option in self.MULTIPLE:
            return parse_multi_values(value)
        return value


def load_config(path, force_fallback):
    if not force_fallback:
        try:
            from platformio.project.config import ProjectConfig  # pylint: disable=import-outside-toplevel
        except ImportError:
            pass
        else:
            return ProjectConfig(path, parse_extra=True, expand_interpolations=True), "ProjectConfig"
    return FallbackConfig(path), "configparser"


# ---------------------------------------------------------------------------
# checks
# ---------------------------------------------------------------------------

def tokens(values):
    """Flags of a multi-value option, split the way SCons ParseFlags does (shlex, posix)"""
    if values is None:
        return []
    if isinstance(values, str):
        values = parse_multi_values(values)
    out = []
    for value in values:
        try:
            out.extend(shlex.split(value))
        except ValueError:
            out.extend(value.split())
    return out


def macros(flags):
    """[('D' | 'U', name, value)] in order"""
    out = []
    pending = None
    for flag in flags:
        if pending:
            out.append(split_macro(pending, flag))
            pending = None
            continue
        if flag in ("-D", "-U"):
            pending = flag[1]
        elif flag.startswith(("-D", "-U")):
            out.append(split_macro(flag[1], flag[2:]))
    return out


def split_macro(kind, text):
    name, sep, value = text.partition("=")
    return kind, name, (value if sep else "1")


def check_env(config, env):
    return check_macros(config, env, FLAG_OPTIONS) + check_macros(config, env, MODULE_FLAG_OPTIONS)


def check_envs(config, path):
    """one env per chip, named after the chip of platformio_<chip>.ini"""
    match = CHIP_RE.search(os.path.basename(path))
    if not match:
        return []
    chip = match.group(1)
    envs = list(config.envs())
    if envs == [chip]:
        return []
    return [f"envs {', '.join(envs) or '(none)'}: exactly one, env:{chip} (head, modules and images in one build)"]


def check_macros(config, env, options):
    problems = []
    section = "env:" + env
    defined = {}
    for option in options:
        for kind, name, value in macros(tokens(config.get(section, option, []))):
            if kind == "U":
                problems.append(
                    f"{option}: -U{name} (PlatformIO puts -U after every -D, the macro ends up undefined; "
                    f"set it once with -D{name}=0 instead)")
                continue
            if name in defined and defined[name][1] != value:
                problems.append(
                    f"{option}: -D{name}={value} after -D{name}={defined[name][1]} ({defined[name][0]}), "
                    f"two values for one macro")
            defined[name] = (option, value)
    return problems


def raw_build_dirs(paths):
    """[(file, section, value)] of every build_dir, read without resolution"""
    out = []
    for path in paths:
        parser = configparser.ConfigParser(inline_comment_prefixes=("#", ";"), interpolation=None)
        with open(path, encoding="utf-8") as f:
            parser.read_file(f, path)
        for section in parser.sections():
            if parser.has_option(section, "build_dir"):
                out.append((path, section, parser.get(section, "build_dir").strip()))
    return out


def norm(path):
    return os.path.normpath(path).replace(os.sep, "/")


def check_build_dir(config, path):
    problems = []
    files = list(getattr(config, "parsed", None) or getattr(config, "_parsed", None) or [path])
    raw = raw_build_dirs(files)
    main = [value for file, section, value in raw if section == "platformio" and norm(file) == norm(path)]

    for file, section, value in raw:
        if section != "platformio":
            problems.append(f"{file}: build_dir in [{section}] (only [platformio] of the chip's ini sets it)")
        elif norm(file) != norm(path):
            problems.append(f"{file}: build_dir in an extra config, the chip's ini sets it")

    value = main[-1] if main else DEFAULT_BUILD_DIR
    if norm(value) not in [norm(x) for x in ALLOWED_BUILD_DIRS]:
        problems.append(
            f"build_dir = {value}{'' if main else ' (not set, PlatformIO default)'}: "
            f"only {', '.join(ALLOWED_BUILD_DIRS)}")
    return value, problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("configs", nargs="+", help="platformio_<chip>.ini, relative to the current directory")
    parser.add_argument("--fallback", action="store_true", help="configparser even when PlatformIO is installed")
    args = parser.parse_args(argv)

    failed = 0
    used_dirs = {}
    for path in args.configs:
        config, how = load_config(path, args.fallback)
        build_dir, problems = check_build_dir(config, path)
        print(f"== {path} (resolved with {how}), build_dir {build_dir}")
        for problem in problems:
            print(f"   FAIL {problem}")
        failed += len(problems)

        for problem in check_envs(config, path):
            print(f"   FAIL {problem}")
            failed += 1

        other = used_dirs.get(norm(build_dir))
        if other:
            print(f"   FAIL build_dir {build_dir} is the build dir of {other} as well, one dir per chip")
            failed += 1
        used_dirs[norm(build_dir)] = path

        for env in config.envs():
            env_problems = check_env(config, env)
            print(f"   {'FAIL' if env_problems else 'ok  '} env:{env}")
            for problem in env_problems:
                print(f"        {problem}")
            failed += len(env_problems)

    if failed:
        print(f"{failed} problem(s)")
        return 1
    print("no problems")
    return 0


if __name__ == "__main__":
    sys.exit(main())
