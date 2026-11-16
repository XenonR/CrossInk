# X4 Pro SDMMC fixes

The `x4-pro` and `x4-pro-debug` builds request an 839 ms SDMMC data-phase
timeout (the hardware maximum at 20 MHz is 838.86075 ms) and use the final
ESP-IDF 5.5 backport of
[#19034](https://github.com/espressif/esp-idf/issues/19034),
[c70648edb6](https://github.com/espressif/esp-idf/commit/c70648edb6adb4004b89477af3991aa3b0a7cbea).
The latter spaces CMD13 status polls starting at 100 microseconds, doubling
up to one FreeRTOS tick, in the write, initialization, and tuning wait paths.
It leaves the existing five-second card-ready deadline intact.

The longer data timeout addresses the card-dependent case discussed in
[ESP-IDF #12968](https://github.com/espressif/esp-idf/issues/12968).
Neither change proves the cause of a particular card's write failures.

## Build

```sh
pio run -e x4-pro
```

The output is `.pio/build/x4-pro/firmware-x4-pro.bin`. Version 1.6.3 is
set in `[crossink]` in `platformio.ini` and receives the usual device suffix.

Arduino supplies precompiled IDF libraries. Merely editing an installed
`sdmmc_host.c` or adding a build define does not change those libraries.
`scripts/patch_sdmmc.py` instead compiles the three affected C files and links
their objects before the original SDK archives. The post-link check rejects
an original archive member or a missing replacement function.

The hook requires the exact pinned IDF revision and verifies SHA-256 hashes
before applying the reviewed patches in `scripts/espidf_patches/`. The first
build uses matching installed IDF sources if available, otherwise downloads
five small files from that exact Espressif commit. Later builds use the cache
inside the environment's build directory. No installed SDK packages or
FreeInk submodule files are changed. An SDK upgrade requires re-reviewing
these patches and their source hashes.

Because the precompiled SDK predates the upstream Kconfig option, the hook
defines its default, `CONFIG_SD_READY_POLL_PERIOD_START_US=100`, only for the
replacement objects. Other device environments do not use the hook.

The X4 Pro keeps its 20 MHz clock (`SDMMC_FREQ_DEFAULT`). Requesting 839 ms
produces 16,780,000 cycles, which the existing low-level setter clamps to the
24-bit maximum of 16,777,215 cycles: exactly 838.86075 ms at 20 MHz. The log
therefore labels 839 ms as the requested value. At 40 MHz the same counter
would allow only about 419 ms; review the timeout if the bus speed changes.

## Verification

```sh
python3 test/scripts/test_sdmmc_patches.py
python3 test/scripts/test_sdmmc_backoff.py .pio/build/x4-pro/espidf-sdmmc/patched
```

On an X4 Pro, install the firmware using the normal flashing method, then
fully power off and start again so the card is reinitialized. The serial
log should include:

```text
CrossInk SDMMC: data timeout request=839 ms, requested clock=20000 kHz
```

Change the language to German, change another setting several times, open
Wi-Fi search, then restart and confirm both settings survive. Check for the
final `/.crosspoint/crossink-settings.json` and for the absence of new
`sdmmc_* timeout`, `0x107`, `Short write`, and persistence errors. The
temporary names `.tmp.tmp` and `.tmp` are intermediate steps in the existing
two nested save operations; a successful save renames them to the final file.
No EPUB cache reset is needed.
