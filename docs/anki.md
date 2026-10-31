# Anki on X4 Pro

## First release contract

The firmware reviews a downloaded due-card batch, not a complete Anki collection.
Each card receives one rating per download, including Again. Anki Desktop owns
scheduling; reconnect to obtain the next batch. Text, line breaks and basic bold
are supported. Images, audio, HTML templates, local scheduling and discovery are
outside this release.

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

Build X4 Pro and its simulator, default C3 and Sticky. Run focused native tests,
simulator Anki scenarios, and existing reading smoke tests. Measure binary size,
internal heap/largest block, PSRAM, and worker stack margins against the baseline.
The app partition is 0x640000 bytes (6,553,600 bytes).
The unchanged v1.6.1 X4 Pro baseline built on 2026-10-31 is 5,585,040 bytes,
leaving 968,560 bytes in that partition (PlatformIO release environment).

On real X4 Pro, test each unmodified plugin version: download, disconnect Wi-Fi,
review all ratings, sleep/restart, reconnect and inspect desktop results. Cut
power during review writes and download activation; exercise SD-full/removal,
lost/partial receipts, USB Drive and return to EPUB reading. Hardware acceptance
must be reported separately from native/simulator results.
