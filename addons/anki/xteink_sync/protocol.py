"""Pure protocol parsing shared by the Anki add-on and its tests."""

from __future__ import annotations

import csv
import hashlib
import io
import json
from dataclasses import dataclass
from typing import Any, Mapping, Optional, Tuple


MAX_BATCH_ID_LENGTH = 128
MAX_DURATION_MS = 3_600_000
MAX_FLAGS_PER_PUSH = 1000
PULL_NDJSON_MIME = "application/x-ndjson"


class ProtocolError(ValueError):
    """A client error that can be returned as a structured HTTP response."""

    def __init__(self, code: str, message: str, status: int = 400) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.status = status


@dataclass(frozen=True)
class Review:
    card_id: int
    ease: int
    answered_at_ms: Optional[int] = None
    duration_ms: int = 0


@dataclass(frozen=True)
class FlagUpdate:
    """Anki user flag 0–7 (0 = clear). X4 toggles 0 ↔ 1 (red)."""

    card_id: int
    flag: int


@dataclass(frozen=True)
class PushBatch:
    batch_id: str
    reviews: Tuple[Review, ...]
    flags: Tuple[FlagUpdate, ...]
    legacy_csv: bool
    derived_batch_id: bool


def encode_pull_ndjson(payload: Mapping[str, Any]) -> bytes:
    """Encode a pull response as one bounded JSON object per line.

    The ESP32-C3 can stream this representation directly to its SD card
    without holding the complete daily card batch in RAM.
    """

    cards = payload.get("cards")
    if not isinstance(cards, list):
        raise ValueError("pull payload cards must be a list")

    decks = payload.get("decks")
    if decks is None:
        decks = []
    if not isinstance(decks, list):
        raise ValueError("pull payload decks must be a list")

    meta = {
        "type": "meta",
        "status": payload.get("status", "success"),
        "protocol_version": payload.get("protocol_version"),
        "pull_id": payload.get("pull_id"),
        "server_time": payload.get("server_time"),
        "card_count": len(cards),
        "decks": decks,
    }
    lines = [json.dumps(meta, ensure_ascii=False, separators=(",", ":"))]
    for card in cards:
        if not isinstance(card, dict):
            raise ValueError("pull payload cards must contain objects")
        lines.append(
            json.dumps(
                {**card, "type": "card"},
                ensure_ascii=False,
                separators=(",", ":"),
            )
        )
    lines.append(
        json.dumps(
            {"type": "end", "card_count": len(cards)},
            ensure_ascii=False,
            separators=(",", ":"),
        )
    )
    return ("\n".join(lines) + "\n").encode("utf-8")


def _integer(value: Any, field: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool):
        raise ProtocolError("invalid_field", f"{field} must be an integer")

    try:
        parsed = int(value)
    except (TypeError, ValueError):
        raise ProtocolError("invalid_field", f"{field} must be an integer")

    if parsed < minimum or parsed > maximum:
        raise ProtocolError(
            "invalid_field",
            f"{field} must be between {minimum} and {maximum}",
        )
    return parsed


def _timestamp_ms(value: Any) -> Optional[int]:
    if value in (None, ""):
        return None

    parsed = _integer(value, "answered_at", 1, 9_999_999_999_999)
    # Ten-digit Unix timestamps are seconds; thirteen-digit values are ms.
    if parsed < 100_000_000_000:
        parsed *= 1000
    return parsed


def _review_from_mapping(item: Any, index: int) -> Review:
    if not isinstance(item, dict):
        raise ProtocolError(
            "invalid_review", f"reviews[{index}] must be a JSON object"
        )

    card_id_value = item.get("card_id", item.get("id"))
    card_id = _integer(card_id_value, f"reviews[{index}].card_id", 1, 2**63 - 1)
    ease = _integer(item.get("ease"), f"reviews[{index}].ease", 1, 4)
    duration_ms = _integer(
        item.get("duration_ms", 0),
        f"reviews[{index}].duration_ms",
        0,
        MAX_DURATION_MS,
    )
    answered_at = item.get("answered_at_ms", item.get("answered_at"))

    return Review(
        card_id=card_id,
        ease=ease,
        answered_at_ms=_timestamp_ms(answered_at),
        duration_ms=duration_ms,
    )


def _flag_from_mapping(item: Any, index: int) -> FlagUpdate:
    if not isinstance(item, dict):
        raise ProtocolError(
            "invalid_flag", f"flags[{index}] must be a JSON object"
        )

    card_id_value = item.get("card_id", item.get("id"))
    card_id = _integer(card_id_value, f"flags[{index}].card_id", 1, 2**63 - 1)
    flag = _integer(item.get("flag", 0), f"flags[{index}].flag", 0, 7)
    return FlagUpdate(card_id=card_id, flag=flag)


def _validate_batch_id(value: Any) -> str:
    if not isinstance(value, str):
        raise ProtocolError("invalid_batch_id", "batch_id must be a string")

    value = value.strip()
    if not value or len(value) > MAX_BATCH_ID_LENGTH:
        raise ProtocolError(
            "invalid_batch_id",
            f"batch_id must contain 1 to {MAX_BATCH_ID_LENGTH} characters",
        )

    if any(ord(char) < 33 or ord(char) > 126 for char in value):
        raise ProtocolError(
            "invalid_batch_id", "batch_id may only contain printable ASCII"
        )
    return value


def _derived_batch_id(body: bytes) -> str:
    return "sha256:" + hashlib.sha256(body).hexdigest()


def _parse_json(body: bytes, header_batch_id: Optional[str]) -> PushBatch:
    try:
        payload = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise ProtocolError("invalid_json", "Request body is not valid UTF-8 JSON")

    if not isinstance(payload, dict):
        raise ProtocolError("invalid_json", "JSON payload must be an object")

    body_batch_id = payload.get("batch_id")
    if body_batch_id is not None and header_batch_id:
        if _validate_batch_id(body_batch_id) != _validate_batch_id(header_batch_id):
            raise ProtocolError(
                "batch_id_mismatch",
                "batch_id differs from X-Xteink-Batch-ID",
            )

    supplied_batch_id = body_batch_id if body_batch_id is not None else header_batch_id
    if supplied_batch_id is None:
        batch_id = _derived_batch_id(body)
        derived = True
    else:
        batch_id = _validate_batch_id(supplied_batch_id)
        derived = False

    reviews_value = payload.get("reviews", [])
    if reviews_value is None:
        reviews_value = []
    if not isinstance(reviews_value, list):
        raise ProtocolError("invalid_reviews", "reviews must be a JSON array")

    flags_value = payload.get("flags", [])
    if flags_value is None:
        flags_value = []
    if not isinstance(flags_value, list):
        raise ProtocolError("invalid_flags", "flags must be a JSON array")

    reviews = tuple(
        _review_from_mapping(item, index)
        for index, item in enumerate(reviews_value)
    )
    flags = tuple(
        _flag_from_mapping(item, index) for index, item in enumerate(flags_value)
    )
    return PushBatch(
        batch_id=batch_id,
        reviews=reviews,
        flags=flags,
        legacy_csv=False,
        derived_batch_id=derived,
    )


def _parse_csv(body: bytes, header_batch_id: Optional[str]) -> PushBatch:
    try:
        text = body.decode("utf-8-sig")
    except UnicodeDecodeError:
        raise ProtocolError("invalid_csv", "CSV body is not valid UTF-8")

    reviews = []
    reader = csv.reader(io.StringIO(text))
    for line_number, row in enumerate(reader, start=1):
        if not row or all(not column.strip() for column in row):
            continue

        if line_number == 1 and row[0].strip().lower() in {"card_id", "id"}:
            continue

        if len(row) < 2 or len(row) > 4:
            raise ProtocolError(
                "invalid_csv",
                f"CSV line {line_number} must contain 2 to 4 columns",
            )

        card_id = _integer(row[0].strip(), f"CSV line {line_number} card_id", 1, 2**63 - 1)
        ease = _integer(row[1].strip(), f"CSV line {line_number} ease", 1, 4)
        answered_at_ms = _timestamp_ms(row[2].strip()) if len(row) >= 3 else None
        duration_ms = (
            _integer(
                row[3].strip(),
                f"CSV line {line_number} duration_ms",
                0,
                MAX_DURATION_MS,
            )
            if len(row) >= 4 and row[3].strip()
            else 0
        )
        reviews.append(
            Review(
                card_id=card_id,
                ease=ease,
                answered_at_ms=answered_at_ms,
                duration_ms=duration_ms,
            )
        )

    if header_batch_id:
        batch_id = _validate_batch_id(header_batch_id)
        derived = False
    else:
        batch_id = _derived_batch_id(body)
        derived = True

    return PushBatch(
        batch_id=batch_id,
        reviews=tuple(reviews),
        flags=(),
        legacy_csv=True,
        derived_batch_id=derived,
    )


def parse_push_payload(
    body: bytes,
    content_type: str,
    header_batch_id: Optional[str] = None,
    max_reviews: int = 500,
    max_flags: int = MAX_FLAGS_PER_PUSH,
    allow_legacy_csv: bool = True,
) -> PushBatch:
    """Parse and validate a JSON review/flag batch (or legacy CSV reviews)."""

    if not body.strip():
        raise ProtocolError("empty_body", "Request body must not be empty")

    mime_type = content_type.partition(";")[0].strip().lower()
    looks_like_json = body.lstrip().startswith(b"{")
    if mime_type == "application/json" or looks_like_json:
        batch = _parse_json(body, header_batch_id)
    else:
        if not allow_legacy_csv:
            raise ProtocolError(
                "unsupported_media_type",
                "Only application/json is accepted",
                status=415,
            )
        batch = _parse_csv(body, header_batch_id)

    if not batch.reviews and not batch.flags:
        raise ProtocolError(
            "empty_batch",
            "At least one review or flag update is required",
        )
    if len(batch.reviews) > max_reviews:
        raise ProtocolError(
            "too_many_reviews",
            f"A push may contain at most {max_reviews} reviews",
            status=413,
        )
    if len(batch.flags) > max_flags:
        raise ProtocolError(
            "too_many_flags",
            f"A push may contain at most {max_flags} flag updates",
            status=413,
        )
    return batch
