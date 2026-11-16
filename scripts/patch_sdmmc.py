"""Rebuild the pinned Arduino SDK's SDMMC objects with reviewed fixes.

X4 Pro uses precompiled IDF libraries (including TinyUSB). Replacing just these
objects preserves that SDK configuration. All work stays in the environment's
build directory; installed packages and the FreeInk SDK are never patched.
"""

import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile
from urllib.request import urlopen


IDF_COMMIT = "87912cd291d68f4319f13695718af6754879a83f"
SOURCES = {
    "esp_driver_sdmmc/src/sdmmc_host.c": "366dcc3e7f72db930632e1e49cebb4ffa8bc080f631c0aecb179cf187315c0ff",
    "esp_driver_sdmmc/src/sdmmc_internal.h": "ac051519173525429b6f233f8bd7877af61755104d00f9cab25c619be2bd55cb",
    "sdmmc/sdmmc_common.c": "075f2c98eaf48a67400427efe6bc00a2f003d70c6871d4d8562ccb781da3e480",
    "sdmmc/sdmmc_sd.c": "927533d6e7861e09de467ce9886c2766686a40edcbae8a98924445f84fa663d4",
    "sdmmc/include/esp_private/sdmmc_common.h": "f70b6099fb141e6a8b6f9c34cd78c7858e9136a66fa89d7f3aa0c2fb25f140ce",
}
PATCHES = ("0001-data-timeout-839ms.patch", "0002-back-off-between-cmd13-polls.patch")
OBJECTS = {
    "esp_driver_sdmmc/src/sdmmc_host.c": ("esp_driver_sdmmc", "sdmmc_host_set_card_clk"),
    "sdmmc/sdmmc_common.c": ("sdmmc", "sdmmc_wait_for_idle"),
    "sdmmc/sdmmc_sd.c": ("sdmmc", "sdmmc_init_sd_wait_data_ready"),
}


def checked_source(data, name):
    if hashlib.sha256(data).hexdigest() != SOURCES[name]:
        raise RuntimeError("Unreviewed ESP-IDF source: " + name)
    return data


def write_if_changed(path, data):
    if path.is_file() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def prepare_sources(build_dir, patch_dir, idf_dir=None):
    build = Path(build_dir)
    upstream = build / "upstream"
    patched = build / "patched"
    for name in SOURCES:
        cached = upstream / name
        if cached.is_file():
            checked_source(cached.read_bytes(), name)
            continue
        installed = Path(idf_dir) / "components" / name if idf_dir else None
        if installed and installed.is_file() and hashlib.sha256(installed.read_bytes()).hexdigest() == SOURCES[name]:
            data = installed.read_bytes()
        else:
            url = "https://raw.githubusercontent.com/espressif/esp-idf/" + IDF_COMMIT + "/components/" + name
            print("Fetching pinned ESP-IDF source: " + name)
            with urlopen(url, timeout=30) as response:
                data = response.read(256 * 1024)
        write_if_changed(cached, checked_source(data, name))

    # Apply in a disposable directory so an incremental build preserves mtimes.
    with tempfile.TemporaryDirectory(prefix="sdmmc-", dir=build) as temporary:
        temporary = Path(temporary)
        for name in SOURCES:
            write_if_changed(temporary / name, (upstream / name).read_bytes())
        process_env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(build.resolve()))
        patches = [str((Path(patch_dir) / name).resolve()) for name in PATCHES]
        for options in (("--check",), ()):
            subprocess.run(["git", "apply", *options, *patches], cwd=temporary,
                           env=process_env, check=True)
        for name in SOURCES:
            write_if_changed(patched / name, (temporary / name).read_bytes())
    return patched


def verify_link_map(map_text):
    text = map_text.replace("\\", "/")
    for name, (archive, symbol) in OBJECTS.items():
        stem = Path(name).stem
        if "lib" + archive + ".a(" + stem + ".c.obj)" in text:
            raise RuntimeError("Unpatched SDMMC archive object was linked: " + stem)
        # Require a live text section, not a name in the cross-reference table.
        pattern = (r" \.text\." + symbol + r"\s+0x(?!0+\s)[0-9a-f]+\s+0x[0-9a-f]+\s+"
                   r"[^\r\n]*espidf-sdmmc/patched/" + re.escape(name) + r"\.o(?:\s|$)")
        if not re.search(pattern, text):
            raise RuntimeError("Patched SDMMC function missing from link map: " + symbol)


def configure(env):
    if env.get("ARDUINO_LIB_COMPILE_FLAG") == "Build":
        return
    if env["PIOENV"] not in ("x4-pro", "x4-pro-debug"):
        raise RuntimeError("SDMMC patches are only reviewed for X4 Pro")
    platform = env.PioPlatform()
    sdk = Path(platform.get_package_dir("framework-arduinoespressif32-libs"))
    if "esp-idf: release/v5.5 " + IDF_COMMIT[:10] not in (sdk / "versions.txt").read_text().splitlines():
        raise RuntimeError("SDMMC patches require the pinned Arduino ESP-IDF SDK; re-review after upgrading")
    root = Path(env.subst("$PROJECT_DIR"))
    build = Path(env.subst("$BUILD_DIR")) / "espidf-sdmmc"
    sources = prepare_sources(build, root / "scripts/espidf_patches",
                              platform.get_package_dir("framework-espidf"))
    compiler = env.Clone()
    compiler.Prepend(CPPPATH=[str(sources / "sdmmc/include")])
    # The precompiled SDK predates the upstream Kconfig option. Use its default.
    compiler.Append(CPPDEFINES=[("CONFIG_SD_READY_POLL_PERIOD_START_US", 100)])
    objects = []
    for name in OBJECTS:
        objects.extend(compiler.Object(str(sources / name)))
    # File nodes are passed verbatim, ahead of archives, so their original
    # members are never extracted. Check that assumption after every link.
    env.Prepend(LIBS=objects)
    env.Depends(env.subst("$PROGPATH"), objects)

    def check_map(source, target, env):
        link_map = Path(env.subst("$BUILD_DIR/${PROGNAME}.map"))
        verify_link_map(link_map.read_text())
        print("Verified patched SDMMC objects: maximum data timeout (~839 ms at 20 MHz) and ESP-IDF #19034")

    env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf",
                      env.VerboseAction(check_map, "Checking patched SDMMC driver objects"))
    print("X4 Pro SDMMC: maximum data timeout (~839 ms at 20 MHz); CMD13 poll backoff from ESP-IDF #19034")


if "Import" in globals():
    Import("env")  # noqa: F821 -- supplied by SCons
    configure(env)  # noqa: F821
