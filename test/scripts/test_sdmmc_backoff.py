#!/usr/bin/env python3
"""Exercise the patched IDF polling functions with a fake clock and SD card.

Usage: python3 test/scripts/test_sdmmc_backoff.py /path/to/espidf-sdmmc/patched
The supplied files must be the prepared build-local sources. Only their actual
function bodies are compiled; timer, delay, logging, and card transport are host
stubs. No installed SDK or source file is changed.
"""

from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def function(source, name):
    match = re.search(
        r"(?ms)^(?:void|esp_err_t) " + re.escape(name) + r"\([^;]*?\)\n\{.*?^\}",
        source,
    )
    if not match:
        raise AssertionError("Cannot find prepared IDF function: " + name)
    return match.group()


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define ESP_OK 0
#define ESP_ERR_TIMEOUT 0x107
#define TEST_CARD_ERROR 0x145
#define READY_STATUS 1U
#define CONFIG_SD_READY_POLL_PERIOD_START_US 100
#define SDMMC_READY_POLL_PERIOD_START_US CONFIG_SD_READY_POLL_PERIOD_START_US
#define portTICK_PERIOD_MS (1000 / TEST_TICK_HZ)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGV(...) ((void)0)

typedef int esp_err_t;
typedef struct { bool spi; } sdmmc_card_t;

static int64_t fake_now_us;
static unsigned timer_reads;
static unsigned jump_on_timer_read;
static unsigned delay_count;
static unsigned yield_count;
static uint32_t first_delays[16];
static bool first_delay_yielded[16];
static unsigned status_calls;
static unsigned ready_on_call;
static unsigned fail_on_call;

static bool host_is_spi(const sdmmc_card_t *card) { return card->spi; }
static bool sdmmc_ready_for_data(uint32_t status) { return status == READY_STATUS; }

static int64_t esp_timer_get_time(void)
{
    if (++timer_reads == jump_on_timer_read) {
        fake_now_us += 5000001;
    }
    return fake_now_us;
}

static void record_delay(uint32_t duration_us, bool yielded)
{
    if (delay_count < 16) {
        first_delays[delay_count] = duration_us;
        first_delay_yielded[delay_count] = yielded;
    }
    ++delay_count;
    yield_count += yielded;
    fake_now_us += duration_us;
}

static void esp_rom_delay_us(uint32_t duration_us)
{
    record_delay(duration_us, false);
}

static void vTaskDelay(uint32_t ticks)
{
    assert(ticks == 1);
    record_delay(portTICK_PERIOD_MS * 1000, true);
}

static esp_err_t sdmmc_send_cmd_send_status(sdmmc_card_t *card, uint32_t *status)
{
    assert(!card->spi);
    assert(++status_calls < 10000); /* Catch a poll storm without hanging. */
    if (status_calls == fail_on_call) {
        return TEST_CARD_ERROR;
    }
    *status = ready_on_call && status_calls >= ready_on_call ? READY_STATUS : 0;
    return ESP_OK;
}

@TIMEOUT@
@FUNCTIONS@

static void reset(void)
{
    fake_now_us = 0;
    timer_reads = 0;
    jump_on_timer_read = 0;
    delay_count = 0;
    yield_count = 0;
    status_calls = 0;
    ready_on_call = 0;
    fail_on_call = 0;
    memset(first_delays, 0, sizeof(first_delays));
    memset(first_delay_yielded, 0, sizeof(first_delay_yielded));
}

static void test_poll_spacing(void)
{
#if TEST_TICK_HZ == 1000
    static const uint32_t expected[] = {100, 200, 400, 800, 1000, 1000, 1000, 1000, 1000};
    const unsigned expected_yields = 5;
#elif TEST_TICK_HZ == 100
    static const uint32_t expected[] = {100, 200, 400, 800, 1600, 3200, 6400, 10000, 10000};
    const unsigned expected_yields = 2;
#else
#error Unexpected test tick rate
#endif
    reset();
    uint32_t period_us = 100;
    for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        sdmmc_poll_delay_and_backoff(&period_us);
        assert(first_delays[i] == expected[i]);
        assert(first_delay_yielded[i] == (expected[i] == portTICK_PERIOD_MS * 1000));
    }
    assert(yield_count == expected_yields);
    assert(period_us == portTICK_PERIOD_MS * 1000);

    /* Configuring a period above one tick must not delay the card for longer. */
    reset();
    period_us = UINT32_MAX;
    sdmmc_poll_delay_and_backoff(&period_us);
    assert(delay_count == 1 && yield_count == 1);
    assert(fake_now_us == portTICK_PERIOD_MS * 1000);
    assert(period_us == portTICK_PERIOD_MS * 1000);
}

static void test_ready_and_error(void)
{
    sdmmc_card_t card = { .spi = false };
    reset();
    assert(sdmmc_wait_for_idle(&card, READY_STATUS) == ESP_OK);
    assert(status_calls == 0 && delay_count == 0 && fake_now_us == 0);

    reset();
    ready_on_call = 3;
    assert(sdmmc_wait_for_idle(&card, 0) == ESP_OK);
    assert(status_calls == 3 && delay_count == 3);
    assert(first_delays[0] == 100 && first_delays[1] == 200 && first_delays[2] == 400);
    assert(fake_now_us == 700 && yield_count == 0);

    reset();
    fail_on_call = 2;
    assert(sdmmc_wait_for_idle(&card, 0) == TEST_CARD_ERROR);
    assert(status_calls == 2 && delay_count == 2 && fake_now_us == 300);
}

static void test_five_second_deadline(void)
{
    sdmmc_card_t card = { .spi = false };
    assert(SDMMC_READY_FOR_DATA_TIMEOUT_US == 5000000);
    reset();
    assert(sdmmc_wait_for_idle(&card, 0) == ESP_ERR_TIMEOUT);
    assert(fake_now_us > 5000000);
    assert(fake_now_us <= 5000000 + portTICK_PERIOD_MS * 1000);
    assert(status_calls == delay_count && status_calls < 6000 && yield_count > 0);

    /* If the deadline already passed, no additional command may reach the bus. */
    reset();
    jump_on_timer_read = 2;
    assert(sdmmc_wait_for_idle(&card, 0) == ESP_ERR_TIMEOUT);
    assert(status_calls == 0 && delay_count == 0);
}

int main(void)
{
    test_poll_spacing();
    test_ready_and_error();
    test_five_second_deadline();
    printf("PASS: %d Hz tick: clamped polling, ready/error paths, five-second deadline\n", TEST_TICK_HZ);
    return 0;
}
'''


def run(prepared_sources):
    source = (prepared_sources / "sdmmc/sdmmc_common.c").read_text()
    header = (prepared_sources / "sdmmc/include/esp_private/sdmmc_common.h").read_text()
    timeout = re.search(r"(?m)^#define SDMMC_READY_FOR_DATA_TIMEOUT_US[^\n]*", header)
    if not timeout:
        raise AssertionError("Cannot find the prepared IDF card-ready timeout")
    functions = "\n\n".join(
        function(source, name)
        for name in ("sdmmc_poll_delay_and_backoff", "sdmmc_wait_for_idle")
    )
    harness = HARNESS.replace("@TIMEOUT@", timeout.group()).replace("@FUNCTIONS@", functions)
    compiler = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
    if not compiler:
        raise RuntimeError("A native C compiler is required")
    with tempfile.TemporaryDirectory(prefix="crossink-sdmmc-backoff-") as temporary:
        directory = Path(temporary)
        test_source = directory / "backoff_test.c"
        test_source.write_text(harness)
        for tick_hz in (1000, 100):
            executable = directory / ("backoff_test_" + str(tick_hz))
            subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                 "-DTEST_TICK_HZ=" + str(tick_hz), str(test_source), "-o", str(executable)],
                check=True, timeout=30,
            )
            subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    run(Path(sys.argv[1]).resolve())
