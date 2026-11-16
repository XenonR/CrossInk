#!/usr/bin/env python3
"""Check pinned-source, incremental-build, and link-map guards without hardware.

Usage: python3 test/scripts/test_sdmmc_patches.py [upstream-source-cache]
Run `pio run -e x4-pro` first to populate the hash-verified source cache.
"""

from pathlib import Path
import runpy
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PATCH = runpy.run_path(str(ROOT / "scripts/patch_sdmmc.py"))
CACHE = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / ".pio/build/x4-pro/espidf-sdmmc/upstream"


class SdmmcPatchTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="crossink-sdmmc-test-")
        self.addCleanup(temporary.cleanup)
        self.build = Path(temporary.name) / "build"
        shutil.copytree(CACHE, self.build / "upstream")

    def prepare(self):
        return PATCH["prepare_sources"](self.build, ROOT / "scripts/espidf_patches")

    def test_verified_backport_and_incremental_build(self):
        source = self.prepare()
        expected = {
            "sdmmc/sdmmc_common.c": "3d7260874f41291560bcd0222720a7f453ba2adb0d4b6b542fce9a11b9d21d60",
            "sdmmc/sdmmc_sd.c": "631534927af7faf50c2f095f01e57499abca5a68cbd7450025aa0fa71266d94f",
            "sdmmc/include/esp_private/sdmmc_common.h": "bea04117bef46d93fc2471b51992e29860c9adf905923e2227f4f94415d6ab7e",
        }
        for name, digest in expected.items():
            self.assertEqual(PATCH["hashlib"].sha256((source / name).read_bytes()).hexdigest(), digest)
        host = (source / "esp_driver_sdmmc/src/sdmmc_host.c").read_text()
        self.assertIn("const uint32_t data_timeout_ms = 839;", host)
        timestamps = {name: (source / name).stat().st_mtime_ns for name in PATCH["SOURCES"]}
        self.prepare()
        self.assertEqual(timestamps, {name: (source / name).stat().st_mtime_ns for name in timestamps})

    def test_modified_input_rejected_before_output_changes(self):
        source = self.prepare()
        host = source / "esp_driver_sdmmc/src/sdmmc_host.c"
        before = host.read_bytes()
        (self.build / "upstream/sdmmc/sdmmc_common.c").write_bytes(b"unknown SDK revision\n")
        with self.assertRaisesRegex(RuntimeError, "Unreviewed"):
            self.prepare()
        self.assertEqual(host.read_bytes(), before)

    def test_link_map_requires_live_patched_objects(self):
        sections = []
        for name, (_, symbol) in PATCH["OBJECTS"].items():
            sections.append(" .text." + symbol + "\n 0x42000000 0x100 "
                            ".pio/build/x4-pro/espidf-sdmmc/patched/" + name + ".o\n")
        good = "".join(sections)
        PATCH["verify_link_map"](good)
        PATCH["verify_link_map"](good.replace(".pio/build", "/project with spaces/.pio/build"))
        PATCH["verify_link_map"](good.replace("/", "\\"))
        for bad in (
            good.replace("0x42000000", "0x00000000", 1),
            good.replace("patched/", "upstream/", 1),
            good + "libesp_driver_sdmmc.a(sdmmc_host.c.obj)\n",
            good + "libsdmmc.a(sdmmc_common.c.obj)\n",
            good + "libsdmmc.a(sdmmc_sd.c.obj)\n",
            "sdmmc_wait_for_idle .pio/build/x4-pro/espidf-sdmmc/patched/sdmmc/sdmmc_common.c.o\n",
        ):
            with self.subTest(map=bad), self.assertRaises(RuntimeError):
                PATCH["verify_link_map"](bad)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
