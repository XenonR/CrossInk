#!/usr/bin/env python3
"""Package the vendored Xteink Sync add-on using only the standard library."""

from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


ADDON_FILES = (
    "__init__.py",
    "config.json",
    "config.md",
    "manifest.json",
    "mdns_advertise.py",
    "protocol.py",
    "textutil.py",
    "user_files/README.txt",
)


def main() -> None:
    root = Path(__file__).resolve().parent
    files = [(root / "xteink_sync" / name, name) for name in ADDON_FILES]
    files.append((root / "LICENSE", "LICENSE"))
    for source, _ in files:
        if not source.is_file():
            raise SystemExit(f"Missing add-on source: {source}")

    output = root / "dist" / "xteink_sync.ankiaddon"
    output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(output, "w", compression=ZIP_DEFLATED) as archive:
        for source, name in files:
            archive.write(source, name)
    print(f"Built {output}")


if __name__ == "__main__":
    main()
