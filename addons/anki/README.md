# Xteink-Anki desktop add-on

This directory contains the standalone desktop companion for CrossInk's Anki
support. It runs inside Anki Desktop and serves due cards and accepts reviews
over the local network. CrossInk already supports its protocol.

## Source and license

- Upstream: [jakovm/xteink-anki](https://github.com/jakovm/xteink-anki).
- Version: **2.5.1**, protocol **3**.
- Pinned commit: [`df2b960dd930af04277525305daa684b0639534b`](https://github.com/jakovm/xteink-anki/tree/df2b960dd930af04277525305daa684b0639534b).
- Copyright (c) 2026 Jakov and contributors; full [MIT license](./LICENSE).

The `xteink_sync/` source directory, both modules in `tests/`, and `LICENSE`
are copied unchanged from that commit. CrossInk adds this README, `build.py`,
and `.gitignore`. The upstream firmware patch and prebuilt installer are not
included. At the pinned commit, the prebuilt installer's `textutil.py` differs
from the source; this builder packages the pinned source.

## Build and install

From the CrossInk repository root, with Python 3 installed:

```sh
python3 addons/anki/build.py
```

The builder uses only Python's standard library and requires no network access.
It writes `addons/anki/dist/xteink_sync.ankiaddon`; this generated directory is
ignored by Git. The script also works from another directory when invoked by
its absolute path.

The archive contains the runtime/config files at its root, the upstream
`user_files/README.txt` template, and the MIT license. Its explicit file list
excludes tests, build tooling, bytecode, and runtime user data. The upstream
`xteink_sync` package identity is preserved so Anki can update the same add-on.

Follow the [Anki flashcards setup](../../README.md#anki-flashcards) to install
the generated file, obtain the LAN address and token, and connect your reader.
Anki Desktop supplies the add-on's Anki/Qt dependencies. The documented manual
connection works without installing the optional mDNS `zeroconf` dependency;
CrossInk requires the address and token to be entered manually.

## Validation

Run the upstream protocol and text-conversion tests from the repository root:

```sh
python3 -m unittest discover -s addons/anki/tests -v
```

These tests do not require Anki or Qt. To check installation, install the built
file in Anki Desktop, restart Anki, and open **Tools → Xteink Status**. Check
that the server starts, shows a LAN address and port, and generates an API token.

On an X4 and an X4 Pro, download due cards through **Home → Anki → Sync**,
review offline, sleep and resume, and sync the ratings back. Confirm the reviews
in Anki Desktop. No cache reset is needed. Keep the intended collection open
and use the [existing review and recovery guidance](../../docs/anki.md).

## Updating the vendored source

1. Choose an upstream commit and confirm that its protocol remains supported by
   CrossInk. Record its version, protocol, and full commit ID above.
2. Replace `xteink_sync/`, the two upstream test modules, and `LICENSE` with
   unchanged files from that commit. Keep local packaging and documentation
   separate, and list any deliberate source changes here.
3. Check upstream imports and update `ADDON_FILES` in `build.py` if runtime files
   were added or removed. Keep the license in the installer and package only
   the upstream user-files template.
4. Run the tests, rebuild, compare the archive contents with the vendored
   files, and repeat the Desktop and hardware checks above. Update the main
   setup documentation and changelog as needed.
