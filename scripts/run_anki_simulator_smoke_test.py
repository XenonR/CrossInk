#!/usr/bin/env python3
"""Exercise offline Anki UI, restart persistence and real HTTP synchronization.

The localhost server implements the documented protocol 2/3 subset. It verifies
request authentication, batches and receipts; it does not run Anki Desktop.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

from run_simulator_smoke_test import CRASH_PATTERNS, DEFAULT_BOOK, ROOT


class AnkiServer(ThreadingHTTPServer):
    """Local contract fixture; never connects to or changes a real collection."""

    def __init__(self, version: int):
        super().__init__(("127.0.0.1", 0), AnkiHandler)
        self.version = version
        self.generation = f"anki-smoke-empty-p{version}"
        self.requests: list[tuple[str, str]] = []
        self.batches: list[str] = []
        self.batch_sizes: list[int] = []
        self.reviews: list[tuple[int, int]] = []
        self.errors: list[str] = []

    def validate(self) -> None:
        expected = [(7000, 1), (7001, 2), (7002, 3), (7000, 1), (7003, 4),
                    (7004, 3), (7000, 3), (7001, 3)]
        expected += [(7000, 1), (7001, 1), (7002, 2), (7000, 4),
                     (7003, 4), (7001, 4), (7004, 4), (7002, 4)]
        if self.errors or self.reviews != expected or self.batch_sizes != [5, 11]:
            raise AssertionError(f"Unexpected protocol {self.version} upload: {self.errors}, "
                                 f"reviews={self.reviews}, batches={self.batch_sizes}")
        if self.requests != [("GET", "/health"), ("POST", "/push"), ("POST", "/push"), ("GET", "/pull")]:
            raise AssertionError(f"Unexpected request order or duplicate upload: {self.requests}")


class AnkiHandler(BaseHTTPRequestHandler):
    server: AnkiServer

    def log_message(self, *_: object) -> None:
        pass

    def respond(self, value: object, status: int = 200) -> None:
        body = json.dumps(value).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def require(self, condition: bool, message: str) -> bool:
        if condition:
            return True
        self.server.errors.append(message)
        self.respond({"error": message}, 400)
        return False

    def do_GET(self) -> None:
        path = urlparse(self.path).path
        self.server.requests.append(("GET", path))
        if path == "/health":
            if not self.require(self.headers.get("Authorization") is None, "Health must be unauthenticated"):
                return
            self.respond({"status": "ok", "protocol_version": self.server.version,
                          "collection_ready": True, "server_time": 1760000000000})
            return
        if not self.require(path == "/pull", "Unexpected GET endpoint"):
            return
        if not self.require(self.headers.get("Authorization") == "Bearer anki-smoke-token", "Missing pull token"):
            return
        if not self.require(self.headers.get("Accept") == "application/x-ndjson", "Missing NDJSON accept header"):
            return
        records = [{"type": "meta", "status": "success", "protocol_version": self.server.version,
                    "pull_id": self.server.generation, "card_count": 0}, {"type": "end", "card_count": 0}]
        body = ("\n".join(json.dumps(record) for record in records) + "\n").encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/x-ndjson")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self) -> None:
        path = urlparse(self.path).path
        self.server.requests.append(("POST", path))
        if not self.require(path == "/push", "Unexpected POST endpoint"):
            return
        if not self.require(self.headers.get("Authorization") == "Bearer anki-smoke-token", "Missing push token"):
            return
        if not self.require(self.headers.get("Content-Type") == "application/json", "Missing JSON content type"):
            return
        length_text = self.headers.get("Content-Length", "")
        if not self.require(length_text.isdecimal() and 0 < int(length_text) <= 8192, "Missing/bounded Content-Length"):
            return
        body = self.rfile.read(int(length_text))
        try:
            batch = json.loads(body)
            batch_id = batch["batch_id"]
            reviews = batch["reviews"]
            if not isinstance(batch_id, str) or not batch_id or not 1 <= len(reviews) <= 32:
                raise ValueError("Invalid batch identity/count")
            if batch_id in self.server.batches:
                raise ValueError("Repeated batch upload")
            for review in reviews:
                card_id, ease = review["card_id"], review["ease"]
                if card_id not in range(7000, 7005) or ease not in range(1, 5):
                    raise ValueError("Invalid card review")
                if not 0 <= review["duration_ms"] <= 3600000:
                    raise ValueError("Invalid review duration")
                self.server.reviews.append((card_id, ease))
        except (ValueError, KeyError, TypeError) as error:
            self.require(False, f"Invalid push body: {error}")
            return
        self.server.batches.append(batch_id)
        self.server.batch_sizes.append(len(reviews))
        self.respond({"status": "success", "protocol_version": self.server.version,
                      "batch_id": batch_id, "processed": len(reviews), "rejected": []})


def run_phase(program: Path, root: Path, env: dict[str, str], phase: str, timeout: int) -> int:
    env["CROSSINK_SIMULATOR_ANKI_SMOKE"] = phase
    marker = {"review": "Anki simulator smoke passed", "restart": "Anki simulator restart passed",
              "repeat": "Anki simulator repeat passed",
              "sync": "Anki simulator sync passed"}[phase]
    try:
        result = subprocess.run([str(program)], cwd=root, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        print(error.stdout.decode() if isinstance(error.stdout, bytes) else error.stdout or "", end="")
        print(f"Anki simulator {phase} phase timed out", file=sys.stderr)
        return 2
    print(result.stdout, end="")
    if result.returncode or marker not in result.stdout or any(p in result.stdout for p in CRASH_PATTERNS):
        print(f"Anki simulator {phase} phase failed (exit {result.returncode})", file=sys.stderr)
        return result.returncode or 2
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env", choices=("simulator", "simulator-X3", "sticky-simulator", "x4-classic-simulator",
                                          "x4-pro-simulator"), default="x4-pro-simulator",
                        help="PlatformIO simulator environment to build and run")
    parser.add_argument("--program", type=Path, help="Run this binary instead of the selected environment's program")
    parser.add_argument("--book", type=Path, default=DEFAULT_BOOK)
    parser.add_argument("--build", action="store_true", help="Build the selected simulator environment first")
    parser.add_argument("--captures", type=Path, help="Save PGM screenshots (messages in all orientations)")
    parser.add_argument("--language", choices=("en", "de"), default="en", help="Anki UI language")
    parser.add_argument("--ui-scale", choices=("small", "large"), default="small", help="Anki UI text size")
    parser.add_argument("--timeout", type=int, default=120, help="Maximum seconds for each simulator process")
    parser.add_argument("--protocol", type=int, choices=(2, 3), action="append", help="Protocol to test (default: both)")
    args = parser.parse_args()
    if args.build:
        result = subprocess.run(["pio", "run", "-e", args.env, "-j1"], cwd=ROOT)
        if result.returncode:
            return result.returncode
    program = args.program or ROOT / ".pio" / "build" / args.env / "program"
    if not program.is_file() or not args.book.is_file():
        print("Simulator binary or EPUB fixture missing; supply --program/--book or build first.", file=sys.stderr)
        return 2
    program = program.resolve()
    for version in args.protocol or (2, 3):
        with tempfile.TemporaryDirectory(prefix=f"crossink-anki-p{version}-") as directory:
            root = Path(directory)
            books = root / "fs_/books"
            books.mkdir(parents=True)
            shutil.copy2(args.book, books / args.book.name)
            env = os.environ.copy()
            env["SDL_VIDEODRIVER"] = "dummy"
            env["CROSSINK_SIMULATOR_ANKI_LANGUAGE"] = args.language.upper()
            env["CROSSINK_SIMULATOR_ANKI_UI_SCALE"] = args.ui_scale
            env["CROSSINK_SIMULATOR_SMOKE_BOOK"] = f"/books/{args.book.name}"
            env["CROSSPOINT_SIM_WIFI_NETWORKS"] = "Anki smoke test:-45:open"
            env["CROSSPOINT_SIM_WIFI_CONNECT"] = "success"
            if args.captures:
                captures = args.captures.resolve() / f"protocol-{version}"
                captures.mkdir(parents=True, exist_ok=True)
                env["CROSSINK_SIMULATOR_ANKI_CAPTURES"] = str(captures)
            for phase in ("review", "restart", "repeat"):
                result = run_phase(program, root, env, phase, args.timeout)
                if result:
                    return result
            server = AnkiServer(version)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            env["CROSSINK_SIMULATOR_ANKI_SERVER"] = f"http://127.0.0.1:{server.server_port}"
            env["CROSSINK_SIMULATOR_ANKI_GENERATION"] = server.generation
            try:
                result = run_phase(program, root, env, "sync", args.timeout)
                if result:
                    return result
                server.validate()
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=5)
            print(f"Protocol {version}: sixteen spaced-learning ratings uploaded once in two batches; empty snapshot installed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
