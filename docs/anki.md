# Anki on X4 Pro

## First release contract

The firmware reviews a downloaded due-card batch, not a complete Anki collection.
Each card receives one rating per download, including Again. Anki Desktop owns
scheduling; reconnect to obtain the next batch. Text, line breaks and basic bold
are supported. Images, audio, HTML templates, local scheduling and discovery are
outside this release.

## Using Anki

1. Install one of the supported, unmodified Xteink-Anki desktop add-ons using
   its installation instructions. Keep Anki Desktop open with the intended
   collection selected.
2. On X4 Pro, open **Home → Anki → Connection**. Enter the desktop server origin
   (for example, `http://192.168.1.20:5050`) and the token shown by the add-on.
   Use the same local network on both devices.
3. Choose **Sync** and connect Wi-Fi. The downloaded due batch is available
   after leaving the sync screen, even without Wi-Fi.
4. Select a deck, read the question, reveal the answer and rate it. Use swipes
   or side buttons to page through long content. Physical Confirm reveals the
   answer, then opens a rating selector; confirm the selected rating to save it.
5. Sync again to upload saved ratings and replace the batch with Desktop's
   current due cards. There is no automatic or background synchronization.

If an upload needs attention, open its recovery action to write a readable
`recovery-<batch-id>.ndjson` report under `/.crossink-anki/`. Check those ratings
in Anki Desktop and manually handle any missing ones before choosing **Handled
in Anki Desktop**. Keep the report as a record. The device retains its original
journal and does not automatically replay uncertain reviews.

Use one Anki collection and avoid reviewing the downloaded cards elsewhere until
their device reviews have been uploaded. The plugin supplies no collection ID or
conflict detection. Address/token edits reconnect the same collection; they must
not redirect pending reviews to another collection.

## Pinned references and compatibility

- CrossInk v1.6.1: `9914146eeae7b46b300f475a16c32426fc02ec1f`.
- FreeInk SDK: `699370183fa3a0e33c9cb83a36f701bbb6022095`; no SDK changes.
- [Xteink-Anki v2.4.0](https://github.com/jakovm/xteink-anki/tree/fbf1672ae290ab98578baf9f0f87db0ff84f5e1f): protocol 2.
- [Xteink-Anki main](https://github.com/jakovm/xteink-anki/tree/df2b960dd930af04277525305daa684b0639534b): add-on 2.5.1, protocol 3.
- [FlashPoint development](https://github.com/zakkbob/flashpoint/tree/028c06a8f06c002147b2f60de043d890796c4732): memory-only CardStore/AnkiConnect reference.
- [CrossNext](https://github.com/manuelx900/CrossNext/tree/e1174b339e9d4e0cc260c932c493e9fb1872bb44): indexed storage and pagination reference; different bridge protocol.

These references have MIT root licenses. The implementation is CrossInk-native;
retain original notices if substantial source is adapted. No reference fonts,
hardware layers, schedulers or media assets are imported.

## Wire contract

`GET /health` is unauthenticated and reports `protocol_version`,
`collection_ready`, and `server_time`. Versions 2 and 3 are supported.

`GET /pull` uses `Authorization: Bearer <token>` and
`Accept: application/x-ndjson`. The response is one meta record, card records,
and an end record. Meta/end counts must match the received cards. IDs remain
positive signed 64-bit integers. The unchanged plugin caps downloads at 1,000
cards; no full-collection, deck-filter or incremental protocol is assumed.

`POST /push` sends JSON with explicit Content-Length, a stable `batch_id`, and
at most 32 ordered reviews. A review contains `card_id`, `ease` (1–4),
`duration_ms` (at most one hour), and optionally `answered_at_ms` when UTC is
valid. The batch ID and payload remain immutable after preparation. Redirects
are disabled. Acknowledgment requires a complete HTTP 200 response, supported
protocol, matching batch ID, `status: success`, the exact processed count, and
no rejections. `sync_requested` only means AnkiWeb sync was scheduled.

The plugin stores only its last 100 completed batch IDs, separately from Anki
review application. Duplicate replies discard the original receipt. Therefore
partial, lost, duplicate, timed-out or otherwise unverifiable receipts pause
uploads. The device retains the affected reviews, exports a readable report,
and offers manual resolution after checking Anki Desktop. It never automatically
replays an uncertain upload.

## Bounds and storage

Transfer allocations are bounded: a 512 KiB record buffer and 128 KiB JSON arena
in PSRAM. Metadata can exceed 400 KiB for 1,000 unusually named decks. Card sides
are bounded to 16 KiB decoded UTF-8 bytes, snapshots to 1,000 cards/decks and
64 MiB wire bytes. Oversized desktop settings fail without replacing installed
content. Review screens need one side buffer, a 125-byte reviewed bitmap and
small pagination state; there is no second framebuffer.

`/.crossink-anki/` is persistent application data, separate from reading caches.
An immutable binary snapshot contains its own card index and deck directory.
Independent versioned journals retain ratings and upload transitions. A rating
is accepted only after write, sync and close succeed. Activation preserves the
previous valid snapshot/journal pair. Recovery validates integrity and generation;
missing/mismatched journals never mean that every card is unreviewed.

FAT rename is not a physical atomicity guarantee. Tests must interrupt each
activation step; real SD power-cut tests remain a release requirement.

## Validation and release acceptance

### Automated results (2026-10-31)

- All 987 native CTest cases pass, including 79 Anki cases. The 79 also pass
  AddressSanitizer and UndefinedBehaviorSanitizer. LeakSanitizer was disabled.
- Storage tests cover all 1,000 cards/decks, a full 16 KiB side, the last bitmap
  bit, failed writes, torn/corrupt journals and interrupted activation/repair.
  Protocol/transport tests cover both supported versions, malformed and oversized
  input, authentication, cancellation and complete/partial/duplicate/lost receipts.
- X4 Pro simulator scenarios pass for protocols 2 and 3, repeated four times
  each. They exercise all eight Home themes, Unicode/bold pagination, orientation,
  touch/physical ratings, failed SD append, process restart, frontlight and return
  to EPUB. The actual sync activity/worker uploads five saved ratings once in
  two immutable batches, installs an empty next batch and tears down Wi-Fi.
- The existing broad EPUB simulator smoke test passes with Anki enabled.
- Scoped PlatformIO cppcheck of `src/anki/` and `src/activities/anki/` reports no
  defects at low, medium or high severity.
- X4 Pro firmware, X4 Pro simulator, default C3 and Sticky build successfully.
  The C3 and Sticky ELFs contain no Anki implementation symbols.

The simulator uses a localhost protocol fixture, not a running Anki Desktop.
Its HTTP shim buffers responses and follows redirects internally; strict
completion, bounds and redirect behavior are additionally tested against the
production HTTP wrapper with native ESP transport stubs. These checks do not
replace testing the unmodified add-ons on a physical device.

Run the normal build and smoke entry points from the repository root:

```sh
pio run -e x4-pro -e x4-pro-simulator -e default -e sticky
cmake -S test -B build/test
cmake --build build/test -j2
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/test --output-on-failure
python3 scripts/run_anki_simulator_smoke_test.py
python3 scripts/run_simulator_smoke_test.py --no-build --env x4-pro-simulator
pio check -e x4-pro -f '+<src/anki/*>' -f '+<src/activities/anki/*>' \
  --fail-on-defect low --fail-on-defect medium --fail-on-defect high
```

If personal PlatformIO overrides replace simulator flags, retain
`-DCROSSINK_APP_CAP_ANKI=1` for `x4-pro-simulator`. Both smoke runners accept
`--program` for a binary in a separate build directory. The Anki runner uses an
isolated temporary SD directory and does not contact a real collection.

### Firmware size

The app partition is 0x640000 bytes (6,553,600 bytes). Release images built with
the same toolchain on 2026-10-31 measure:

| X4 Pro image | Binary bytes | Partition space remaining |
| --- | ---: | ---: |
| Unchanged v1.6.1 baseline | 5,585,040 | 968,560 |
| Anki implementation | 5,639,200 | 914,400 |

The increase is 54,160 bytes. Static RAM reported by the X4 Pro build is
104,500 / 327,680 bytes; this does not measure runtime heap or PSRAM use.

### Physical X4 Pro acceptance (pending)

No physical-device or live Anki Desktop results are claimed. Before release:

1. Test each pinned, unmodified add-on with **Home → Anki → Connection → Sync**.
   Confirm the intended collection, download, disconnect Wi-Fi, review all four
   ratings, sleep/restart, reconnect and inspect the actual desktop ratings.
2. Interrupt power during journal writes and snapshot activation. Exercise SD
   full/removal, cancelled downloads and desktop timeout/crash. Accepted local
   ratings must survive; failed writes must keep the current card ungraded;
   interrupted downloads must preserve a matching installed snapshot/journal.
3. Lose or partially deliver a push receipt, then restart. Confirm uploads pause,
   export the SD report, reconcile in Desktop, and use **Handled in Anki Desktop**.
   Check that uncertain events are retained and never automatically replayed.
4. Repeat review/sync/exit cycles while recording internal free heap and largest
   allocatable block, PSRAM free/largest block and the sync worker stack margin
   (`ANKI` completion log). Simulator heap/stack placeholders are not measurements.
5. Exit to EPUB reading, use frontlight and Home, then enter/leave USB Drive.
   No EPUB cache reset is required by this feature. Keep `/.crossink-anki/` intact
   while investigating recovery; it contains review evidence, not a reading cache.

FAT power-loss behavior and resource margins require real SD/hardware results;
native fault injection and simulator results are reported separately above.
