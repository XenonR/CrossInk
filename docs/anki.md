# Anki on X4 Pro

## Review contract

The firmware reviews a downloaded due-card batch, not a complete Anki collection.
Again and Hard keep a card in the local review queue; Good and Easy complete it.
Completed decks can be repeated offline, and every saved rating is uploaded in
order. Anki Desktop owns long-term scheduling; reconnect to obtain the next batch.
The existing desktop add-on needs no changes. Text, line breaks and basic bold
are supported. Images, audio, HTML templates, timed learning steps, local FSRS and
discovery are outside this release.

| Rating | Local effect |
| --- | --- |
| Again | Repeat after two intervening saved answers in this deck |
| Hard | Repeat after five intervening saved answers in this deck |
| Good / Easy | Complete the card for this pass |

Ready repeats alternate with untouched cards when both are available. Repeats
are selected by earliest eligible answer count across Again and Hard, with ties
resolved by original card order. Untouched cards keep their downloaded order.
If there is no other available card, the earliest waiting repeat appears
immediately. Spacing is therefore a target: alternation can delay a repeat, and
small decks can shorten its gap. A one-card deck repeats Again or Hard immediately.
Only successfully saved answers in this deck advance the gap; revealing, paging,
leaving the screen or reviewing another deck does not. This is a small offline
queue, not Desktop's configurable learning-step scheduler.

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
   Again and Hard return the card later in the same pass. The deck finishes only
   when every card has received Good or Easy.
5. When a deck is complete, choose **Repeat deck** to review all its downloaded
   cards again. You can also select a completed deck from the Anki list to reach
   this action. Touch or physical Confirm starts a pass; Back returns to the list.
   Additional ratings will sync to Anki Desktop, including ratings of cards
   previously marked Good or Easy. Leaving, sleeping or restarting retains
   accepted ratings and the same next-card order; selecting an unfinished deck
   resumes its queue.
6. Sync again to upload all saved ratings and replace the batch with Desktop's
   current due cards. There is no automatic or background synchronization.

If an upload needs attention, open its recovery action to write a readable
`recovery-<batch-id>.ndjson` report under `/.crossink-anki/`. Check those ratings
in Anki Desktop and manually handle any missing ones before choosing **Handled
in Anki Desktop**. Keep the report as a record. The device retains its original
journal and does not automatically replay uncertain reviews.

Use one Anki collection and avoid reviewing the downloaded cards elsewhere until
their device reviews have been uploaded. The plugin supplies no collection ID or
conflict detection. Address/token edits reconnect the same collection. Connection
changes require confirmation while downloaded cards or pending ratings remain;
reviewing a downloaded card after a connection edit must still send its rating to
the same collection.

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
content. Review screens need one side buffer, a 125-byte completion bitmap,
small pagination state and a fallible `uint32_t` queue array of at most 4,000
bytes for 1,000 cards. The array is reused throughout a review and freed when
the store closes; fixed queue fields and allocator overhead are additional.
Home and sync do not allocate the array. It uses the default allocator, which
prefers internal RAM for this size on X4 Pro. There is no second framebuffer.

`/.crossink-anki/` is persistent application data, separate from reading caches.
An immutable binary snapshot contains its own card index and deck directory.
Independent versioned journals retain ratings and upload transitions. A rating
is accepted only after write, sync and close succeed. Activation preserves the
previous valid snapshot/journal pair. Recovery validates integrity and generation;
missing/mismatched journals never mean that every card is unreviewed.

Journal version 3 adds learning-rating events. Again and Hard record a retry;
Good and Easy complete the card. On review entry, sequential snapshot and journal
scans rebuild deck membership, the deck's saved-answer counter and retry order,
including ratings of cards completed later. Acknowledgment affects upload state,
not retry eligibility or completion. A successful sync replaces the local queue
with Desktop's next downloaded batch; a failed download preserves the current
batch and queue.

Versions 1 and 2 remain readable. Their legacy rating events keep the original
meaning: every rating completed a card, including Again and Hard. Version 2's
durable deck-restart marker continues to reset only the selected deck's current
pass, preserving earlier reviews and acknowledgments. Older journals upgrade
through a synced copy before new learning events are written, preserving the
original, review sequences, prepared payloads and batch IDs. Card snapshots stay
at version 1. Do not clear Anki data to enable learning repeats.

FAT rename is not a physical atomicity guarantee. Tests must interrupt each
activation step; real SD power-cut tests remain a release requirement.

## Source review and resource tradeoffs (2026-11-01)

This historical review describes the implementation before the learning queue;
its operation counts and measurements are that earlier baseline. The review
covers the Anki store, protocol decoder, HTTP transaction path,
connection settings, sync worker, review/deck activities, text layout and their
tests. Fixes stay within Anki; the SDK, shared persistence and general HTTP
behavior are unchanged. No long-term scheduler, collection format, settings or
dependency was added.

The main correctness changes are:

- Protocol control fields now compare complete JSON strings. Previously a value
  such as `success\u0000junk` matched `success` through a C-string comparison,
  including in upload acknowledgments. Malformed receipts leave reviews pending
  for reconciliation.
- Physical card input waits until the requested screen has rendered. Rapid
  Confirm releases cannot reveal, select and grade through unseen states. A
  Confirm held during touch grading also has its release consumed on the next
  card. Back and error dismissal remain available.
- Changing the connection with unreviewed downloaded cards now requires the
  same collection confirmation already used for saved ratings. Those cards
  retain their original collection ownership even before their first rating.
- Any staged card/deck I/O failure cancels that import. Retrying an append can
  no longer publish partially updated deck counts. An activation retry may
  reuse only a byte-exact, empty matching journal after checked sync/close;
  nonempty, mismatched or damaged journals remain untouched.
- Configuration is read once after checking its size, using a fallible buffer
  of at most 2,048 bytes. Parsing rejects trailing records and literal NULs;
  backup recovery validates the file before restoring it. Failed initialization
  leaves the sync client unavailable, and rejected download data is distinguished
  from a failed HTTP connection.

| Change | Device work / RAM | SD and firmware storage |
| --- | --- | --- |
| Upload body workspace | 8,192 → 4,096 bytes, explicitly in PSRAM; the maximum supported encoded body is 3,227 bytes | Same request and journal bytes |
| Fresh deck-list row, 1,000 cards | One file open and 256 bytes read, previously two opens and 40,256 bytes read | Uses the existing directory and 125-byte reviewed bitmap; no new index |
| Next-card search | One open for the scan, previously up to 1,000; reviewed index entries remain skipped | Same 40-byte card records and bounded read volume; no retained card list |
| Rating-focus and tap redraws | Reuse existing page/line offsets; one validity boolean, no new heap | No page index or SD writes; e-ink painting still occurs |
| Snapshot and journal CRC | Reuse the existing uzlib CRC, with two nibble steps instead of eight bit steps per byte | Identical CRC32 and binary formats; reuse the firmware's existing constant table |
| Connection load | One open/read/close; at most 2 KiB of input off the task stack | Same version 1 JSON and backup names |

These are allocation bounds and operation counts, not measured hardware latency
or battery improvements. Native tests assert the read/open bounds and verify
the unchanged CRC bytes across transfer-block boundaries. Review duration and
SD sync costs still depend on the card and real device.

The X4 Pro prebuilt SDK uses `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` and
prefers internal RAM for allocations at or below that limit. Shrinking an
ordinary allocation to exactly 4 KiB would therefore increase internal-RAM
pressure. The upload buffer explicitly uses PSRAM, like the parser workspaces;
failure is checked instead of silently falling back to internal RAM. It is
serialized once per upload batch, rather than used in the review render path.

The snapshot deliberately reserves a 256-byte deck slot per downloaded card.
Its size remains `128 + 296 * card_count + front/back UTF-8 bytes`. The journal
retains 64-byte events after its 96-byte header; ratings, repeat markers and
upload transitions keep their existing write/sync/close guarantees. Old journals,
backups and recovery reports remain as evidence. No compaction is introduced;
version 3 upgrades change the header while preserving historical event bytes.

Backward paging still rebuilds preceding pages instead of allocating a page
history. Contextual text measurement is retained for kerning and shaping.
The 512 KiB line buffer and 128 KiB JSON arena remain in PSRAM, since shrinking
them could reject supported Unicode deck metadata. No second framebuffer is
allocated. Anki stays disabled on the C3 and Sticky environments.

Remaining boundaries are explicit: the desktop add-on cannot prove collection
identity or exactly-once application after a lost receipt, so uncertain uploads
still require manual reconciliation. Cancellation is checked between synchronous
HTTP operations; a stalled operation can wait for its 75-second network timeout.
Connection saves still use the shared JSON persistence helper; its Arduino
`String` serialization does not explicitly check allocation failure, and that
shared implementation is outside this Anki-only change. Transient import failures
can share the generic rejected-download result. Physical SD durability and runtime
heap/PSRAM/stack margins still need the acceptance checks below.

## Validation and release acceptance

### Learning queue regression results (2026-11-01)

- All 1,036 native CTest cases pass, including 128 Anki cases. The 128 also pass
  AddressSanitizer and UndefinedBehaviorSanitizer with leak detection disabled.
- Twenty-two new storage cases cover two/five-answer spacing, alternation,
  earliest-due and ordinal ties, rating changes, small decks, deck independence,
  deterministic resume, legacy versions 1/2, interrupted upgrades, torn events,
  failed writes, allocation failure and immutable prepared uploads.
- Sync tests verify 60 ordered retry ratings across 32/28-review batches and
  preserved queue order after an acknowledged upload followed by a failed pull.
- Protocols 2 and 3 pass review/restart/repeat/sync in English/small and
  German/large. Each run uploads all 16 original/retry/repeated grades once,
  in immutable batches of 5 and 11, then installs an empty next batch.
- The broad EPUB simulator smoke test passes with Anki enabled. Native allocation
  instrumentation verifies a single 4,000-byte array for 1,000 cards, reused
  through selection and grading. Fixed store fields and allocator overhead are
  additional; physical-device heap and stack margins remain unmeasured.
- Scoped Anki cppcheck reports no low, medium or high defects. X4 Pro firmware,
  X4 Pro simulator, default C3 and Sticky builds pass. C3 and Sticky ELFs contain
  no Anki implementation symbols.
- The X4 Pro image is 5,648,592 bytes, an increase of 1,200 bytes from the previous
  5,647,392-byte image, leaving 905,008 bytes in its OTA partition. Static internal
  RAM remains 104,500 bytes; the ELF section totals confirm the unchanged static
  allocation. These figures do not measure runtime heap/PSRAM or task-stack margins.

The earlier automated results below describe the previous one-rating-per-card
implementation and its resource baseline.

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

### Offline repeat regression results (2026-11-01)

- All 997 native CTest cases pass, including 89 Anki cases. The 42 storage/sync
  cases also pass AddressSanitizer and UndefinedBehaviorSanitizer with leak
  detection disabled.
- Repeat tests cover independent decks, partial-pass resume, immutable prepared
  uploads, version 1 upgrades at every storage checkpoint, torn/corrupt restart
  events, failed writes and 65,536 pending reviews. Sequential upload reads are
  checked against a bounded total byte count.
- Protocols 2 and 3 pass the simulator's review/restart/repeat/sync phases in
  English and German at both UI sizes. Each run uploads five original and five
  repeated ratings in order, once each, across two immutable batches. The test
  exercises both repeat entry points and the held-Confirm release guard.
- Completion captures fit in all four orientations for English/small and
  German/large. The broad EPUB simulator smoke and scoped Anki cppcheck pass.
- X4 Pro firmware/simulator, default C3 and Sticky builds pass. C3 and Sticky
  firmware contain no Anki implementation symbols. The X4 Pro image is
  5,643,856 bytes, leaving 909,744 bytes in its OTA partition; reported static
  RAM remains 104,500 bytes. These figures do not measure runtime heap margins.

Physical-device scheduling, SD power-loss behavior and resource margins remain
subject to the hardware acceptance steps below.

### Source review regression results (2026-11-01)

- All 1,012 native CTest cases pass, including 104 Anki cases. The 104 also pass
  AddressSanitizer and UndefinedBehaviorSanitizer with leak detection disabled.
  Fifteen additional native cases cover malformed control strings/configuration,
  staging failures at 16 I/O boundaries, activation retry/re-sync, preserved
  journal evidence, bounded SD operations and the unchanged cross-block CRC.
- Protocols 2 and 3 pass review/restart/repeat/sync in English and German at both
  UI sizes. The final simulator flow checks cached-card connection confirmation
  with zero pending reviews, cancellation preserving credentials, rapid Confirm
  bursts, held Confirm during touch grading, and pagination reuse/invalidation.
  Every run uploads the original and repeated ratings once, in order.
- English/small and German/large confirmation captures fit in portrait and
  landscape. The existing broad EPUB smoke test passes with Anki enabled.
- Scoped Anki PlatformIO cppcheck reports no low, medium or high defects.
- X4 Pro firmware/simulator, default C3 and Sticky builds pass. C3 and Sticky
  ELFs contain no Anki implementation symbols. The X4 Pro image is 5,647,392
  bytes, leaving 906,208 bytes in its OTA partition; reported static RAM remains
  104,500 bytes. Runtime heap/PSRAM margins and hardware latency were not measured.

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
   Before the first rating, edit the connection and cancel the collection
   confirmation: the original address/token must remain. Rapid Confirm presses
   must wait for each new screen. Hold Confirm while touch-grading a nonfinal
   card, then release it: the next question must remain unrevealed.
   Use a deck of at least eight cards: Again should return after two saved answers
   and Hard after five when enough other cards are available. Mix both ratings
   and untouched cards to check alternation and earliest-due ordering. Sleep
   before a repeat is due, then resume: the next-card order must be unchanged.
   Confirm a one-card deck repeats Again/Hard immediately. Finish every card with
   Good or Easy; only then should the completion screen appear.
   Complete a deck and repeat it using physical Confirm, leave midway, sleep or
   restart, then resume and finish. Open the completed deck from the list and
   repeat using touch. Sync and inspect Desktop's review history: every pass's
   ratings, including every Again/Hard retry, must appear in order. Confirm that
   the new downloaded batch replaces the local queue. Release a Confirm held
   during the last touch rating; it must leave the completion screen visible.
2. Interrupt power during journal writes and snapshot activation. Exercise SD
   full/removal, cancelled downloads and desktop timeout/crash. Accepted local
   ratings must survive; failed writes must keep the current card ungraded;
   interrupted downloads must preserve a matching installed snapshot/journal.
   Include deck-restart writes, learning-rating writes and version 1/2 journal
   upgrades to version 3. Earlier ratings and prepared uploads must survive;
   legacy ratings retain their completed-card meaning. A failed download after
   an acknowledged upload must retain the current local retries.
3. Lose or partially deliver a push receipt, then restart. Confirm uploads pause,
   export the SD report, reconcile in Desktop, and use **Handled in Anki Desktop**.
   Check that uncertain events are retained and never automatically replayed.
4. Repeat review/sync/exit cycles while recording internal free heap and largest
   allocatable block, PSRAM free/largest block and the sync worker stack margin
   (`ANKI` completion log). Simulator heap/stack placeholders are not measurements.
   Include a 1,000-card deck and repeated review entry/exit to check the at-most
   4,000-byte queue allocation is released and heap/stack margins remain safe.
5. Exit to EPUB reading, use frontlight and Home, then enter/leave USB Drive.
   No EPUB cache reset is required by this feature. Keep `/.crossink-anki/` intact
   while investigating recovery; it contains review evidence, not a reading cache.

FAT power-loss behavior and resource margins require real SD/hardware results;
native fault injection and simulator results are reported separately above.
