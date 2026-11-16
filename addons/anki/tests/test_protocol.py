import importlib.util
import json
import pathlib
import sys
import unittest


PROTOCOL_PATH = pathlib.Path(__file__).parents[1] / "xteink_sync" / "protocol.py"
SPEC = importlib.util.spec_from_file_location("xteink_protocol", PROTOCOL_PATH)
assert SPEC and SPEC.loader
PROTOCOL = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = PROTOCOL
SPEC.loader.exec_module(PROTOCOL)

ProtocolError = PROTOCOL.ProtocolError
parse_push_payload = PROTOCOL.parse_push_payload
encode_pull_ndjson = PROTOCOL.encode_pull_ndjson


class ParsePushPayloadTests(unittest.TestCase):
    def test_pull_ndjson_has_bounded_records(self):
        encoded = encode_pull_ndjson(
            {
                "status": "success",
                "protocol_version": 2,
                "pull_id": "pull-123",
                "server_time": 1_785_000_000,
                "decks": [
                    {"id": "1", "name": "Greek", "card_count": 1},
                    {"id": "2", "name": "Spanish", "card_count": 1},
                ],
                "cards": [
                    {
                        "id": "1700000000000",
                        "front": "line one\nline two",
                        "back": "Antwort",
                        "is_learning": True,
                        "deck_id": "1",
                        "deck_name": "Greek",
                    },
                    {
                        "id": "1700000000001",
                        "front": "Question",
                        "back": "Answer",
                        "is_learning": False,
                        "deck_id": "2",
                        "deck_name": "Spanish",
                    },
                ],
            }
        )

        lines = encoded.decode("utf-8").splitlines()
        self.assertEqual(len(lines), 4)
        meta = json.loads(lines[0])
        self.assertEqual(meta["card_count"], 2)
        self.assertEqual(meta["decks"][0]["name"], "Greek")
        self.assertEqual(json.loads(lines[1])["front"], "line one\nline two")
        self.assertEqual(json.loads(lines[1])["deck_name"], "Greek")
        self.assertEqual(json.loads(lines[2])["type"], "card")
        self.assertEqual(json.loads(lines[3]), {"type": "end", "card_count": 2})

    def test_pull_ndjson_omits_empty_decks_list_when_missing(self):
        encoded = encode_pull_ndjson(
            {
                "status": "success",
                "protocol_version": 2,
                "pull_id": "pull-legacy",
                "server_time": 1,
                "cards": [
                    {
                        "id": "1",
                        "front": "Q",
                        "back": "A",
                        "is_learning": False,
                    }
                ],
            }
        )
        meta = json.loads(encoded.decode("utf-8").splitlines()[0])
        self.assertEqual(meta["decks"], [])

    def test_json_batch(self):
        body = json.dumps(
            {
                "batch_id": "pull-123",
                "reviews": [
                    {
                        "card_id": "1700000000000",
                        "ease": 3,
                        "answered_at": 1_785_000_000,
                        "duration_ms": 1250,
                    },
                    {
                        "id": 1700000000001,
                        "ease": 1,
                        "answered_at_ms": 1_785_000_060_000,
                    },
                ],
            }
        ).encode()

        batch = parse_push_payload(body, "application/json")

        self.assertEqual(batch.batch_id, "pull-123")
        self.assertFalse(batch.legacy_csv)
        self.assertFalse(batch.derived_batch_id)
        self.assertEqual(len(batch.reviews), 2)
        self.assertEqual(batch.reviews[0].answered_at_ms, 1_785_000_000_000)
        self.assertEqual(batch.reviews[0].duration_ms, 1250)
        self.assertEqual(batch.reviews[1].ease, 1)

    def test_repeated_card_ids_are_allowed_for_learning_steps(self):
        body = json.dumps(
            {
                "batch_id": "learning-session",
                "reviews": [
                    {"card_id": 1700000000000, "ease": 1},
                    {"card_id": 1700000000000, "ease": 3},
                ],
            }
        ).encode()

        batch = parse_push_payload(body, "application/json")

        self.assertEqual(
            [review.card_id for review in batch.reviews],
            [1700000000000, 1700000000000],
        )

    def test_csv_with_header_and_optional_fields(self):
        body = (
            b"card_id,ease,answered_at,duration_ms\r\n"
            b"1700000000000,4,1785000000,900\r\n"
        )

        batch = parse_push_payload(
            body,
            "text/csv; charset=utf-8",
            header_batch_id="pull-456",
        )

        self.assertTrue(batch.legacy_csv)
        self.assertEqual(batch.batch_id, "pull-456")
        self.assertEqual(batch.reviews[0].ease, 4)
        self.assertEqual(batch.reviews[0].duration_ms, 900)

    def test_csv_without_id_gets_stable_hash(self):
        body = b"1700000000000,3\n"

        first = parse_push_payload(body, "text/csv")
        second = parse_push_payload(body, "text/csv")

        self.assertTrue(first.derived_batch_id)
        self.assertEqual(first.batch_id, second.batch_id)
        self.assertTrue(first.batch_id.startswith("sha256:"))

    def test_header_and_json_batch_id_must_match(self):
        body = json.dumps(
            {
                "batch_id": "body-id",
                "reviews": [{"card_id": 1700000000000, "ease": 3}],
            }
        ).encode()

        with self.assertRaisesRegex(ProtocolError, "differs"):
            parse_push_payload(
                body,
                "application/json",
                header_batch_id="header-id",
            )

    def test_invalid_ease_is_rejected(self):
        body = json.dumps(
            {
                "batch_id": "bad-ease",
                "reviews": [{"card_id": 1700000000000, "ease": 5}],
            }
        ).encode()

        with self.assertRaises(ProtocolError) as context:
            parse_push_payload(body, "application/json")

        self.assertEqual(context.exception.code, "invalid_field")

    def test_review_limit_is_enforced(self):
        body = json.dumps(
            {
                "batch_id": "too-many",
                "reviews": [
                    {"card_id": 1700000000000, "ease": 3},
                    {"card_id": 1700000000001, "ease": 3},
                ],
            }
        ).encode()

        with self.assertRaises(ProtocolError) as context:
            parse_push_payload(body, "application/json", max_reviews=1)

        self.assertEqual(context.exception.status, 413)

    def test_json_flags_only_batch(self):
        body = json.dumps(
            {
                "batch_id": "flags-only",
                "reviews": [],
                "flags": [
                    {"card_id": 1700000000000, "flag": 1},
                    {"card_id": "1700000000001", "flag": 0},
                ],
            }
        ).encode()

        batch = parse_push_payload(body, "application/json")

        self.assertEqual(len(batch.reviews), 0)
        self.assertEqual(len(batch.flags), 2)
        self.assertEqual(batch.flags[0].flag, 1)
        self.assertEqual(batch.flags[1].card_id, 1700000000001)
        self.assertEqual(batch.flags[1].flag, 0)

    def test_json_reviews_and_flags(self):
        body = json.dumps(
            {
                "batch_id": "both",
                "reviews": [{"card_id": 1, "ease": 3}],
                "flags": [{"card_id": 1, "flag": 1}],
            }
        ).encode()

        batch = parse_push_payload(body, "application/json")
        self.assertEqual(len(batch.reviews), 1)
        self.assertEqual(len(batch.flags), 1)

    def test_empty_reviews_and_flags_rejected(self):
        body = json.dumps({"batch_id": "empty", "reviews": [], "flags": []}).encode()

        with self.assertRaises(ProtocolError) as context:
            parse_push_payload(body, "application/json")

        self.assertEqual(context.exception.code, "empty_batch")

    def test_invalid_flag_value_rejected(self):
        body = json.dumps(
            {
                "batch_id": "bad-flag",
                "flags": [{"card_id": 1, "flag": 9}],
            }
        ).encode()

        with self.assertRaises(ProtocolError) as context:
            parse_push_payload(body, "application/json")

        self.assertEqual(context.exception.code, "invalid_field")

    def test_pull_ndjson_preserves_flag(self):
        encoded = encode_pull_ndjson(
            {
                "status": "success",
                "protocol_version": 3,
                "pull_id": "pull-flag",
                "server_time": 1,
                "cards": [
                    {
                        "id": "1",
                        "front": "Q",
                        "back": "A",
                        "flag": 1,
                        "is_learning": False,
                    }
                ],
            }
        )
        card = json.loads(encoded.decode("utf-8").splitlines()[1])
        self.assertEqual(card["flag"], 1)
        self.assertEqual(card["type"], "card")



if __name__ == "__main__":
    unittest.main()
