#!/usr/bin/env python3
"""Lint of the PlatformIO configs of the CI (.github/workflows/esp32.yml, first job).

    python scripts/ci_ini_lint.py platformio_esp8266.ini platformio_esp32.ini platformio_beken.ini

Run from code/ (extra_configs are relative to it, same as for `pio run -c ...`). platformio_<chip>.ini are the chips,
any other config (a separate project of the release) takes part in the library, macro and
platform checks. Fails on:
- `-U<NAME>` in the build_flags / build_src_flags of an env: PlatformIO puts every -U after every -D, so
  `-UX -DX=1` leaves X undefined (06.11: options silently off)
- a macro defined twice with different values in one env's resolved build_flags + build_src_flags
  (`-DX` counts as `-DX=1`)
- ARDUINO_ARCH_<...> in the flags of an Arduino framework env: the framework defines it
- ESPURNA_IMAGE of a chip anywhere but in build_flags, or other than "<chip>" (one form on every chip)
- a build dir other than .pio/build_esp8266, .pio/build_esp32, .pio/build_beken (one per chip): `build_dir` of
  [platformio] missing (PlatformIO's default .pio/build) or anything else, and `build_dir` in any other section
- envs: exactly one per chip, named after it (platformio_<chip>.ini has env:<chip> and nothing else, its extra
  configs included): the head, every module and the release images are one build
- custom_module_flags (the macros of the module objects, scripts/module_link.py): -U or two values for one macro
- a library or a platform with a version anywhere but libraries.ini (owner 11-12: one version, written once; the
  configs name them as ${libraries.<name>} / ${platforms.<name>}), and an entry of libraries.ini that is not one exact
  version (a release, a commit, a tag of our forks)
- a library in the lib_deps of two configs with two different versions (kept as the second line of defence)
- the libraries of the modules: a library of custom_module_libs that is a lib_deps library or one of the project's
  lib/ and is not in lib_ignore (scripts/module_link.py compiles it for the modules, the head's LDF must not); a
  lib_deps library that is a module library on one chip and not on another chip that has it (one lib_ignore)
- a platform or a platform package not pinned to one version (a version range, a bare name, a branch); a
  separate project takes the platform of its chip (PROJECT_CHIPS)

Values are resolved with PlatformIO's ProjectConfig when it can be imported (same resolution as the build: extends,
[env], ${section.option}, extra_configs), otherwise with configparser and the same rules re-implemented here.
"""

import argparse
import configparser
import glob
import json
import os
import re
import shlex
import sys

ALLOWED_BUILD_DIRS = (".pio/build_esp8266", ".pio/build_esp32", ".pio/build_beken")
DEFAULT_BUILD_DIR = ".pio/build"
FLAG_OPTIONS = ("build_flags", "build_src_flags")
MODULE_FLAG_OPTIONS = ("custom_module_flags",)
CHIP_RE = re.compile(r"platformio_([a-z0-9]+)\.ini$")
# the separate projects of the release (the folder of their platformio.ini) and the chip whose platform they take
PROJECT_CHIPS = {}


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

    MULTIPLE = {"build_flags", "build_src_flags", "extends", "extra_configs", "lib_deps", "lib_ignore",
                "platform_packages"}

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
            # relative to the project, as PlatformIO takes them (../libraries.ini)
            base = os.path.dirname(path)
            for pattern in parse_multi_values(self.parser.get("platformio", "extra_configs")):
                for item in sorted(glob.glob(os.path.join(base, os.path.expanduser(pattern)))):
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
            # extra_configs are relative to the project of the config (../libraries.ini), as
            # for `pio run` inside it: read it from its own folder
            folder = os.path.dirname(os.path.abspath(path))
            here = os.getcwd()
            os.chdir(folder)
            try:
                config = ProjectConfig(os.path.basename(path), parse_extra=True, expand_interpolations=True)
            finally:
                os.chdir(here)
            return config, "ProjectConfig"
    return FallbackConfig(path), "configparser"


def option(config, section, name, default=None):
    """a resolved option, `default` when the section or the option is missing (ProjectConfig raises for a section)"""
    try:
        value = config.get(section, name, default)
    except Exception:  # pylint: disable=broad-except
        return default
    return default if value is None else value


def values(config, section, name):
    """a multi-value option as a list"""
    value = option(config, section, name, [])
    if isinstance(value, str):
        value = parse_multi_values(value)
    return list(value or [])


def chip_of(path):
    """esp8266 / esp32 / beken of platformio_<chip>.ini, None for any other config"""
    match = CHIP_RE.search(os.path.basename(path))
    return match.group(1) if match else None


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


def check_env(config, env, chip=None):
    return (check_macros(config, env, FLAG_OPTIONS) + check_macros(config, env, MODULE_FLAG_OPTIONS)
            + check_arch_macros(config, env) + check_image(config, env, chip))


def env_macros(config, env):
    """[(option, kind, name, value)] of build_flags and build_src_flags"""
    out = []
    for name in FLAG_OPTIONS:
        for kind, macro, value in macros(tokens(option(config, "env:" + env, name, []))):
            out.append((name, kind, macro, value))
    return out


def check_arch_macros(config, env):
    # only an Arduino framework defines ARDUINO_ARCH_<...>; without one (espidf, no Arduino 2026-11-17) the head's
    # own layer defines it for the libraries
    if "arduino" not in str(option(config, "env:" + env, "framework", "")):
        return []
    return [f"{name}: -D{macro}, the framework defines it"
            for name, kind, macro, _ in env_macros(config, env) if kind == "D" and macro.startswith("ARDUINO_ARCH_")]


def check_image(config, env, chip):
    """ESPURNA_IMAGE of a chip: once, in build_flags, "<chip>" (the backslashes of the ini are gone after shlex)"""
    if not chip:
        return []
    found = [(name, value) for name, kind, macro, value in env_macros(config, env)
             if kind == "D" and macro == "ESPURNA_IMAGE"]
    if found == [("build_flags", f'"{chip}"')]:
        return []
    shown = ", ".join(f"{name} {value}" for name, value in found) or "none"
    return [f"ESPURNA_IMAGE: {shown}; once, in build_flags: -DESPURNA_IMAGE=\\\"{chip}\\\" (one form on every chip)"]


def check_envs(config, path):
    """one env per chip, named after the chip of platformio_<chip>.ini"""
    chip = chip_of(path)
    if not chip:
        return []
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


LIB_GIT_RE = re.compile(r"(?:https?://|git@)[^/]+[/:](?:[^/]+)/([^/#@]+?)(?:\.git)?(?:#(.+))?$")


def library(dep):
    """(name, version) of a lib_deps entry: owner/Name@spec, Name@spec, a git URL#ref; None for a bare name"""
    dep = dep.strip()
    match = LIB_GIT_RE.match(dep)
    if match:
        return match.group(1).lower(), match.group(2) or "(default branch)"
    name, sep, spec = dep.partition("@")
    if not sep:
        return None
    return name.split("/")[-1].strip().lower(), spec.strip()


def lib_deps(config):
    """lib_deps of every env, and [common] shared_lib_deps of the ESP8266 (installed once by scripts/pio_pre.py)"""
    out = []
    for env in config.envs():
        out += values(config, "env:" + env, "lib_deps")
    return out + values(config, "common", "shared_lib_deps")


def lib_names(config):
    return {item[0] for item in (library(dep) for dep in lib_deps(config)) if item}


def project_libs(path):
    """{folder name: library name} of the project's lib/ next to the config, lower case"""
    out = {}
    folder = os.path.join(os.path.dirname(os.path.abspath(path)), "lib")
    if not os.path.isdir(folder):
        return out
    for entry in sorted(os.listdir(folder)):
        manifest = os.path.join(folder, entry, "library.json")
        if os.path.isfile(manifest):
            with open(manifest, encoding="utf-8") as f:
                out[entry.lower()] = str(json.load(f).get("name", entry)).lower()
    return out


def check_module_libs(configs):
    """[problem]: the libraries of the modules are out of the head's LDF, and the same ones on every chip"""
    problems = []
    module_libs = {}  # chip -> lib_deps libraries in custom_module_libs and lib_ignore
    declared = {}     # chip -> lib_deps libraries
    for path, config in configs:
        chip = chip_of(path)
        if not chip:
            continue
        own = project_libs(path)
        names = lib_names(config)
        declared[chip] = names
        module_libs[chip] = set()
        for env in config.envs():
            section = "env:" + env
            mods = {name.lower() for name in " ".join(values(config, section, "custom_module_libs")).split()}
            ignored = {name.lower() for name in values(config, section, "lib_ignore")}
            module_libs[chip] |= {name for name in names if name in mods and name in ignored}
            for name in sorted(mods):
                wanted = own.get(name) or (name if name in names else None)
                if wanted and wanted not in ignored:
                    problems.append(f"{path} env:{env}: custom_module_libs {name} without {wanted} in lib_ignore, "
                                    f"the head's LDF builds it as well")

    for name in sorted(set().union(*module_libs.values()) if module_libs else ()):
        modular = sorted(chip for chip in module_libs if name in module_libs[chip])
        for chip in sorted(declared):
            if (name in declared[chip]) and (chip not in modular):
                problems.append(f"lib {name}: a module library (custom_module_libs + lib_ignore) on "
                                f"{', '.join(modular)}, not on {chip}")
    return problems


# one exact version: `name @ 1.2.3`, a release zip, a git URL at a commit, or the esptool folder of <core>/tools (the
# one esptool 5.4.0 of both chips, [platforms] esptool of libraries.ini)
PINNED_RE = re.compile(r"^(?:[\w./ -]+\s*@\s*)?(?:\d+(?:\.\d+)+|https?://\S+/releases/download/\S+\.zip|"
                       r"https?://\S+#[0-9a-f]{7,40}|file://\S*/tools/tool-esptoolpy)$")


def pinned(spec):
    spec = spec.strip()
    if ("@" not in spec) and not spec.startswith("http"):
        return False
    return bool(PINNED_RE.match(spec))


def platform_of(config):
    """(platform, [platform_packages]) of the first env"""
    envs = list(config.envs())
    if not envs:
        return "", []
    section = "env:" + envs[0]
    return option(config, section, "platform", "").strip(), values(config, section, "platform_packages")


def check_platforms(configs):
    """[problem]: every platform pinned; the separate projects on the platform of their chip"""
    problems = []
    by_chip = {}
    for path, config in configs:
        platform, packages = platform_of(config)
        if not platform:
            continue
        for spec in [platform] + packages:
            if not pinned(spec):
                problems.append(f"{path}: {spec} is not pinned to one version")
        chip = chip_of(path)
        if chip:
            by_chip[chip] = (platform, packages)

    for path, config in configs:
        chip = PROJECT_CHIPS.get(os.path.basename(os.path.dirname(os.path.abspath(path))))
        if (not chip) or (chip not in by_chip):
            continue
        platform, packages = platform_of(config)
        if platform != by_chip[chip][0]:
            problems.append(f"{path}: platform {platform}, the {chip} build: {by_chip[chip][0]}")
    return problems


def check_lib_versions(configs):
    """[problem] for every library two configs take in different versions"""
    seen = {}
    for path, config in configs:
        for dep in lib_deps(config):
            item = library(dep)
            if item:
                seen.setdefault(item[0], {})[path] = item[1]

    problems = []
    for name, versions in sorted(seen.items()):
        if len(set(versions.values())) > 1:
            problems.append(f"lib {name}: " + ", ".join(f"{v} ({p})" for p, v in sorted(versions.items())))
    return problems


SHARED_INI = "libraries.ini"
VERSIONED_OPTIONS = ("lib_deps", "shared_lib_deps", "platform", "platform_packages", "git_toolchain_package")
# one exact version, or a tag of a git URL (our forks are released by tags)
GIT_TAG_RE = re.compile(r"^(?:https?://|git@)\S+#[\w.+-]+$")


def raw_files(path):
    """the config and its extra configs, as written"""
    out = [path]
    parser = configparser.ConfigParser(inline_comment_prefixes=("#", ";"), interpolation=None, strict=False)
    with open(path, encoding="utf-8") as f:
        parser.read_file(f, path)
    if parser.has_option("platformio", "extra_configs"):
        base = os.path.dirname(os.path.abspath(path))
        for pattern in parse_multi_values(parser.get("platformio", "extra_configs")):
            for item in sorted(glob.glob(os.path.join(base, os.path.expanduser(pattern)))):
                out.append(item)
    return out


def check_shared_versions(paths):
    """[problem]: a library or a platform with a version anywhere but libraries.ini; libraries.ini not exact"""
    problems = []
    seen = set()
    for path in paths:
        for file in raw_files(path):
            key = norm(file)
            if key in seen:
                continue
            seen.add(key)
            parser = configparser.ConfigParser(inline_comment_prefixes=("#", ";"), interpolation=None, strict=False)
            with open(file, encoding="utf-8") as f:
                parser.read_file(f, file)
            shared = os.path.basename(file) == SHARED_INI
            for section in parser.sections():
                for name in parser.options(section):
                    value = parser.get(section, name)
                    if shared:
                        # lwip, mbedtls: the version the platform / SDK brings built in (MAJOR.MINOR.REVISION)
                        bare = name.startswith(("lwip", "mbedtls")) and re.match(r"^\d+\.\d+\.\d+$", value)
                        if section in ("libraries", "platforms") and not (pinned(value) or GIT_TAG_RE.match(value)
                                                                          or bare):
                            problems.append(f"{file}: [{section}] {name} = {value}: not one exact version")
                        continue
                    if name not in VERSIONED_OPTIONS:
                        continue
                    for item in parse_multi_values(value):
                        if (("@" in item) or ("://" in item)) and not item.startswith("${"):
                            problems.append(f"{file}: [{section}] {name}: {item} - a version of its own, "
                                            f"take ${{libraries.<name>}} / ${{platforms.<name>}} of {SHARED_INI}")
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("configs", nargs="+",
                        help="platformio_<chip>.ini and the platformio.ini of the other projects, relative to the "
                             "current directory")
    parser.add_argument("--fallback", action="store_true", help="configparser even when PlatformIO is installed")
    args = parser.parse_args(argv)

    failed = 0
    used_dirs = {}
    loaded = []
    for path in args.configs:
        config, how = load_config(path, args.fallback)
        loaded.append((path, config))
        chip = chip_of(path)
        if not chip:
            print(f"== {path} (resolved with {how}), a project of its own")
        else:
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
            env_problems = check_env(config, env, chip)
            print(f"   {'FAIL' if env_problems else 'ok  '} env:{env}")
            for problem in env_problems:
                print(f"        {problem}")
            failed += len(env_problems)

    for title, problems in (
            (f"versions only in {SHARED_INI}", check_shared_versions(args.configs)),
            (f"libraries of {len(loaded)} configs", check_lib_versions(loaded)),
            ("libraries of the modules", check_module_libs(loaded)),
            ("platforms", check_platforms(loaded))):
        print(f"== {title}")
        for problem in problems:
            print(f"   FAIL {problem}")
        failed += len(problems)

    if failed:
        print(f"{failed} problem(s)")
        return 1
    print("no problems")
    return 0


if __name__ == "__main__":
    sys.exit(main())
