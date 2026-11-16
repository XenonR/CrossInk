"""Plain-text / XFD conversion for Anki cards (no aqt dependency).

XFD (*Xteink Flashcard Dialect*) maps a small HTML/Markdown subset to
device-ready plain text: tables, lists, titles, quotes, separators.

Phase B bold: HTML/Markdown emphasis becomes STX/ETX markers (U+0002 / U+0003)
in the pull payload. The X4 draws mixed regular/bold runs; it also accepts
visible ``**…**`` as a toggle fallback. Inline code markers are stripped.
"""

from __future__ import annotations

import html
import re
from typing import Dict, Iterable, List, Sequence, Tuple

EMPTY_SIDE_PLACEHOLDER = "[…]"

# Zero-width style markers for the X4 (not shown as glyphs).
BOLD_ON = "\x02"
BOLD_OFF = "\x03"

# Vector table block markers (X4 draws 1px grid; not shown as glyphs).
# Must NOT be Python splitlines() boundaries (\x1c–\x1e break str.splitlines).
# Wire format (one table):
#   \x04table c=<cols> r=<rows> f=<flags>\n
#   cell\x06cell\x06...\n   (r times)
#   \x04/table\n
# flags: C=conjugation (no outer box), H=header row (rule under first row),
#        empty/B=simple box with light rules.
TABLE_RS = "\x04"  # EOT — block marker
CELL_US = "\x06"  # ACK — cell separator
TABLE_START_PREFIX = f"{TABLE_RS}table "
TABLE_END_LINE = f"{TABLE_RS}/table"

# Vector figure blocks (stem / stress / timeline) — same RS, drawn with drawLine.
# Wire (stem):
#   \x04fig t=stem\n
#   stem\x06ending\x06result\n
#   \x04/fig\n
# Wire (stress): s=0-based stress index
#   \x04fig t=stress s=2\n
#   syl0\x06syl1\x06…\n
#   \x04/fig\n
# Wire (timeline): m=0-based marker index; optional 2nd line = event label
#   \x04fig t=timeline m=1\n
#   lab0\x06lab1\x06lab2\n
#   Aorist?\n
#   \x04/fig\n
FIG_START_PREFIX = f"{TABLE_RS}fig "
FIG_END_LINE = f"{TABLE_RS}/fig"
MAX_FIG_CELLS = 8

# Layout budgets for e-ink (character columns, not pixels).
#
# Device font scale (Anki settings) multiplies glyph advance:
#   1 = klein  1.0× (UI_12)
#   2 = mittel native UI_18 @ 1×
#   3 = groß   UI_12 @ 2×
# Content width ≈ screen − 2×(sidePad 20 + 4) → portrait ~432px, landscape ~752px.
# Conversion runs on the Mac without the device scale → always target **groß+portrait**
# so table lines never wrap (wrap destroys ASCII / vector row structure on the X4).
MAX_TABLE_COLUMNS = 4
MAX_TABLE_ROWS = 12
WIDTH_BUDGET_SMALL = 50   # klein 1×, portrait (comfortable)
WIDTH_BUDGET_MEDIUM = 36  # mittel ~1.5×, portrait
WIDTH_BUDGET_LARGE = 26   # groß 2×, portrait (~27 cols usable; stay under)
DEFAULT_WIDTH_BUDGET = WIDTH_BUDGET_LARGE
COL_GAP = 2

_BOLD_OPEN_TAG_RE = re.compile(r"<(?:b|strong)\b[^>]*>", flags=re.IGNORECASE)
_BOLD_CLOSE_TAG_RE = re.compile(r"</(?:b|strong)\s*>", flags=re.IGNORECASE)

_ANSWER_SEPARATOR_RE = re.compile(
    r"<hr\b[^>]*\bid\s*=\s*([\"']?)answer\1[^>]*>",
    flags=re.IGNORECASE,
)
# Also match Anki's common unquoted / class-based answer separators.
_ANSWER_SEPARATOR_LOOSE_RE = re.compile(
    r"<hr\b[^>]*(?:\bid\s*=\s*([\"']?)answer\1|class\s*=\s*([\"'][^\"']*\banswer\b[^\"']*[\"']))[^>]*>",
    flags=re.IGNORECASE,
)
_SCRIPT_STYLE_RE = re.compile(
    r"<(script|style)\b[^>]*>.*?</\1\s*>",
    flags=re.IGNORECASE | re.DOTALL,
)
_STRUCTURAL_TAG_RE = re.compile(
    r"</?(?:br|div|p|li|tr|h[1-6]|section|article|blockquote|td|th)\b[^>]*>",
    flags=re.IGNORECASE,
)
_CLOZE_RE = re.compile(r"\{\{c\d+::(.*?)(?:::(.*?))?\}\}", flags=re.DOTALL)
_SOUND_RE = re.compile(r"\[sound:[^\]]+\]", flags=re.IGNORECASE)
_RUBY_RT_RE = re.compile(r"<rt\b[^>]*>.*?</rt>", flags=re.IGNORECASE | re.DOTALL)
_TAG_RE = re.compile(r"<[^>]+>")
_IMG_TAG_RE = re.compile(r"<img\b[^>]*>", flags=re.IGNORECASE)
_ATTR_RE = re.compile(
    r"""\b(alt|title|aria-label)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+))""",
    flags=re.IGNORECASE,
)
_TABLE_RE = re.compile(r"<table\b[^>]*>.*?</table>", flags=re.IGNORECASE | re.DOTALL)
_LIST_RE = re.compile(r"<(ul|ol)\b[^>]*>(.*?)</\1\s*>", flags=re.IGNORECASE | re.DOTALL)
_LI_RE = re.compile(r"<li\b[^>]*>(.*?)</li\s*>", flags=re.IGNORECASE | re.DOTALL)
_TR_RE = re.compile(r"<tr\b[^>]*>(.*?)</tr\s*>", flags=re.IGNORECASE | re.DOTALL)
_CELL_RE = re.compile(r"<t[dh]\b[^>]*>(.*?)</t[dh]\s*>", flags=re.IGNORECASE | re.DOTALL)
_BLOCKQUOTE_RE = re.compile(
    r"<blockquote\b[^>]*>(.*?)</blockquote\s*>",
    flags=re.IGNORECASE | re.DOTALL,
)
_H12_RE = re.compile(r"<h([12])\b[^>]*>(.*?)</h\1\s*>", flags=re.IGNORECASE | re.DOTALL)
_H36_RE = re.compile(r"<h([3-6])\b[^>]*>(.*?)</h\1\s*>", flags=re.IGNORECASE | re.DOTALL)
_HR_RE = re.compile(r"<hr\b[^>]*/?>", flags=re.IGNORECASE)
_BR_RE = re.compile(r"<br\s*/?>", flags=re.IGNORECASE)
_BLOCK_OPEN_CLOSE_RE = re.compile(
    r"</?(?:p|div|section|article)\b[^>]*>",
    flags=re.IGNORECASE,
)

_MD_HEADING_RE = re.compile(r"^(#{1,6})\s+(.*)$")
_MD_UL_RE = re.compile(r"^([-*+])\s+(.*)$")
_MD_OL_RE = re.compile(r"^(\d+)\.\s+(.*)$")
_MD_HR_RE = re.compile(r"^(-{3,}|\*{3,}|_{3,})\s*$")
_MD_SEP_CELL_RE = re.compile(r"^:?-{1,}:?$")
_BOLD_AST_RE = re.compile(r"\*\*(.+?)\*\*", flags=re.DOTALL)
_BOLD_UND_RE = re.compile(r"__(.+?)__", flags=re.DOTALL)
_CODE_RE = re.compile(r"`([^`]+)`")

# Common field names across German/English note types.
_FRONT_FIELD_NAMES = (
    "Front",
    "Text",
    "Question",
    "Frage",
    "Vorderseite",
    "Word",
    "Wort",
    "Expression",
    "Term",
    "Begriff",
    "Greek",
    "Griechisch",
    "Lemma",
    "Prompt",
)
_BACK_FIELD_NAMES = (
    "Back",
    "Extra",
    "Answer",
    "Antwort",
    "Rückseite",
    "Meaning",
    "Definition",
    "Übersetzung",
    "Translation",
    "German",
    "Deutsch",
    "English",
    "Notes",
    "Notiz",
)


def strip_style_markers(text: str) -> str:
    """Remove STX/ETX bold markers (for tests and plain comparisons)."""

    if not text:
        return ""
    return text.replace(BOLD_ON, "").replace(BOLD_OFF, "")


def _lemma_head_from_line(line: str) -> str:
    """First-line lemma candidate: drop bold markers and trailing (pronunciation)."""

    s = strip_style_markers(line or "").strip()
    if not s:
        return ""
    # "**lemma**  (pron)" or "lemma  (pron)" after marker strip
    s = re.sub(r"\s*\([^)]*\)\s*$", "", s).strip()
    # Drop leading article for matching bare form too (handled by caller with both)
    return re.sub(r"\s+", " ", s)


def strip_memory_lemma_echo(text: str) -> str:
    """On card backs, drop full lemma restated after ※ (already in the head line).

    Example::
        **πίνω**  (píno)

        ※ πίνω ~ potion
    becomes::
        **πίνω**  (píno)

        ※ ~ potion
    """

    if not text or "※" not in text:
        return text or ""

    lines = str(text).split("\n")
    lemma = ""
    for line in lines:
        head = _lemma_head_from_line(line)
        if head:
            lemma = head
            break
    if len(lemma) < 2:
        return text

    bare = lemma
    for art in ("ο ", "η ", "το ", "οι ", "αι ", "τα ", "Ο ", "Η ", "Το ", "Οι ", "Αι ", "Τα "):
        if bare.startswith(art):
            bare = bare[len(art) :].strip()
            break
    candidates = [lemma]
    if bare and bare != lemma and len(bare) >= 2:
        candidates.append(bare)

    art_opt = r"(?:(?:ο|η|το|οι|αι|τα)\s+)?"
    out: List[str] = []
    for line in lines:
        stripped = line.lstrip()
        if not stripped.startswith("※"):
            out.append(line)
            continue
        # Preserve indent + icon
        m = re.match(r"^(\s*※\s*)(.*)$", line)
        if not m:
            out.append(line)
            continue
        prefix, body = m.group(1), m.group(2)
        for word in candidates:
            wp = re.escape(word).replace(r"\ ", r"\s+")
            body = re.sub(
                rf"^{art_opt}{wp}(?=\s|[~'\"(\-/>]|$)\s*",
                "",
                body,
                count=1,
                flags=re.IGNORECASE,
            )
            body = re.sub(
                rf"\s*>\s*{art_opt}{wp}\b(?:\s*['\"][^'\"]{{0,40}}['\"])?\s*$",
                "",
                body,
                flags=re.IGNORECASE,
            )
            body = re.sub(
                rf"\b(?:related\s+to|as\s+in)\s+{art_opt}{wp}\b",
                "",
                body,
                flags=re.IGNORECASE,
            )
        body = re.sub(r"\s{2,}", " ", body).strip()
        if body.startswith("~"):
            body = "~ " + body.lstrip("~").strip()
        out.append(prefix + body if body else prefix.rstrip())
    return "\n".join(out)


def strip_table_markers(text: str) -> str:
    """Remove vector-table control chars (for tests / plain previews)."""

    if not text:
        return ""
    return text.replace(TABLE_RS, "").replace(CELL_US, " | ")


def _display_width(text: str) -> int:
    """Approximate glyph columns (markers have zero width)."""

    return len(strip_style_markers(text or ""))


def _clamp(text: str, limit: int) -> str:
    if limit > 0 and len(text) > limit:
        return text[: max(0, limit - 1)].rstrip() + "…"
    return text


def _img_placeholder(tag: str) -> str:
    for match in _ATTR_RE.finditer(tag):
        value = match.group(2) or match.group(3) or match.group(4) or ""
        value = html.unescape(value).strip()
        if value:
            return f"[{value}]"
    # Keep a visible marker so media-only cards are not blank.
    if re.search(r"\bsrc\s*=", tag, flags=re.IGNORECASE):
        return "[Bild]"
    return ""


def _html_bold_tags_to_markers(text: str) -> str:
    text = _BOLD_OPEN_TAG_RE.sub(BOLD_ON, text)
    text = _BOLD_CLOSE_TAG_RE.sub(BOLD_OFF, text)
    return text


def _md_bold_to_markers(text: str) -> str:
    """Convert ``**…**`` / ``__…__`` to STX/ETX (non-greedy, non-nested)."""

    def wrap(match: re.Match[str]) -> str:
        inner = match.group(1)
        if not inner:
            return ""
        return f"{BOLD_ON}{inner}{BOLD_OFF}"

    text = _BOLD_AST_RE.sub(wrap, text)
    text = _BOLD_UND_RE.sub(wrap, text)
    return text


def _apply_inline_styles(text: str) -> str:
    """Bold → markers; strip inline code backticks (content kept)."""

    text = _html_bold_tags_to_markers(text)
    text = _md_bold_to_markers(text)
    text = _CODE_RE.sub(r"\1", text)
    return text


def _compact_blank_lines(text: str) -> str:
    # Do not .strip() away style markers at line edges; only trim spaces/tabs.
    # ASCII box table lines must keep internal padding for column alignment.
    # Vector table blocks (\x04…) and cell rows with \x06 must stay intact.
    # Stacked table indents ("  Key: value") must keep leading spaces.
    compact: List[str] = []
    for raw in text.split("\n"):
        if raw.startswith(TABLE_RS) or CELL_US in raw:
            # Table block header/end or cell row — keep as-is (rstrip only).
            line = raw.rstrip(" \t")
        elif raw.startswith("+") or raw.startswith("|"):
            # Keep internal spaces; only rstrip trailing pad noise.
            line = raw.rstrip(" \t")
        elif raw.startswith("  ") or raw.startswith("\t"):
            # Preserve indent; collapse only internal runs after the indent.
            indent_len = len(raw) - len(raw.lstrip(" \t"))
            indent = raw[:indent_len]
            rest = re.sub(r"[ \t]+", " ", raw[indent_len:]).strip(" \t")
            line = (indent + rest) if rest else ""
        else:
            line = re.sub(r"[ \t]+", " ", raw).strip(" \t")
        if line:
            compact.append(line)
        elif compact and compact[-1]:
            compact.append("")
    return "\n".join(compact).strip(" \t\n")


def _preprocess_media(text: str) -> str:
    text = _CLOZE_RE.sub(lambda match: match.group(1), text)
    text = _SOUND_RE.sub("", text)
    text = _RUBY_RT_RE.sub("", text)
    text = _IMG_TAG_RE.sub(lambda match: _img_placeholder(match.group(0)), text)
    text = _SCRIPT_STYLE_RE.sub("", text)
    return text


def _inline_to_text(fragment: str) -> str:
    """HTML fragment → single-line-ish plain text (no block structure)."""

    if not fragment:
        return ""
    text = str(fragment)
    text = _preprocess_media(text)
    text = _apply_inline_styles(text)
    text = _BR_RE.sub(" ", text)
    text = _TAG_RE.sub(" ", text)
    text = html.unescape(text).replace("\r\n", "\n").replace("\xa0", " ")
    text = re.sub(r"[ \t\n]+", " ", text).strip(" \t")
    return text


# Canonical 3rd-person pronoun short forms (match greek-anki-helper XFD).
PRONOUN_3SG_SHORT = "αυτός/ή/ό"
PRONOUN_3PL_SHORT = "αυτοί/ές/ά"
_PRONOUN_3PL_LONG_RE = re.compile(
    r"αυτο[ίι]\s*(?:/|,|·)\s*αυτ[έε]ς\s*(?:/|,|·)\s*αυτ[άα]"
    r"|αυτοι\s*(?:/|,|·)\s*αυτες\s*(?:/|,|·)\s*αυτα"
)
_PRONOUN_3SG_LONG_RE = re.compile(
    r"αυτ[οό]ς\s*(?:/|,|·)\s*αυτ[ήη]\s*(?:/|,|·)\s*αυτ[οό](?![ίιηήέε])"
    r"|αυτος\s*(?:/|,|·)\s*αυτη\s*(?:/|,|·)\s*αυτο(?![ίι])"
)
_PRONOUN_3SG_MID_RE = re.compile(
    r"αυτ[οό]ς\s*/\s*αυτ[ήη]\s*/\s*[οό]"
    r"|αυτ[οό]ς\s*/\s*[ήη]\s*/\s*αυτ[οό]"
)
_PRONOUN_3PL_MID_RE = re.compile(
    r"αυτο[ίι]\s*/\s*αυτ[έε]ς\s*/\s*[άα]"
    r"|αυτο[ίι]\s*/\s*[έε]ς\s*/\s*αυτ[άα]"
)


# UI_12 / UI_18: no Unicode arrows (missing glyph / blank box on X4).
# Leave list bullets (•) and em dashes alone — other code paths / fonts use them.
_DEVICE_ARROW_MAP = str.maketrans({
    "\u2192": ">",  # →
    "\u2190": "<",  # ←
    "\u21d2": ">",  # ⇒
    "\u21d0": "<",  # ⇐
    "\u27f6": ">",  # ⟶
    "\u2794": ">",  # ➔
    "\u279c": ">",  # ➜
    "\u27a1": ">",  # ➡
})


def _normalize_device_punctuation(text: str) -> str:
    """Replace arrow glyphs missing from X4 UI fonts (blank box otherwise)."""
    if not text:
        return text
    return text.translate(_DEVICE_ARROW_MAP)


def normalize_pronoun_short_forms(text: str) -> str:
    """Map verbose 3rd-person pronoun lists to αυτός/ή/ό and αυτοί/ές/ά."""
    if not text:
        return text
    out = str(text)
    out = _PRONOUN_3PL_LONG_RE.sub(PRONOUN_3PL_SHORT, out)
    out = _PRONOUN_3PL_MID_RE.sub(PRONOUN_3PL_SHORT, out)
    out = _PRONOUN_3SG_LONG_RE.sub(PRONOUN_3SG_SHORT, out)
    out = _PRONOUN_3SG_MID_RE.sub(PRONOUN_3SG_SHORT, out)
    return out


_PERSON_HEADER_RE = re.compile(
    r"^(person|person\s*form|pers\.?|pronoun|pron\.?)$",
    flags=re.IGNORECASE,
)
_PERSON_LABEL_RE = re.compile(
    r"^(?:1(?:st)?|2(?:nd)?|3(?:rd)?|first|second|third|"
    r"εγώ|εσύ|αυτός|αυτή|αυτό|εμείς|εσείς|αυτοί|αυτές|αυτά)"
    r"(?:\s|/|\.|$)",
    flags=re.IGNORECASE,
)


def _strip_person_column(rows: List[List[str]]) -> List[List[str]]:
    """Drop a leading Person/pronoun column from conjugation-style tables.

    Keeps a short 1/2/3 corner label when the Person cell encodes person number;
    otherwise removes the column entirely (Sg|Pl grids need no Person form).
    """
    if not rows or not rows[0]:
        return rows
    header0 = strip_style_markers(rows[0][0] or "").strip()
    body = rows[1:] if len(rows) > 1 else []
    looks_person = bool(_PERSON_HEADER_RE.match(header0))
    if not looks_person and body:
        looks_person = sum(
            1
            for r in body
            if r and _PERSON_LABEL_RE.match(strip_style_markers(r[0] or "").strip())
        ) >= max(2, len(body) // 2)
    if not looks_person or max(len(r) for r in rows) < 2:
        return rows

    def person_number(cell: str) -> str:
        """Map Person cell → '1'|'2'|'3'."""
        t = strip_style_markers(cell or "").strip().lower()
        if re.match(r"^(1|1st|first)\b", t):
            return "1"
        if re.match(r"^(2|2nd|second)\b", t):
            return "2"
        if re.match(r"^(3|3rd|third)\b", t):
            return "3"
        # Greek / romanized pronouns
        if t.startswith("εγ") or t.startswith("eg") or "εμεί" in t or t.startswith("εμει") or "eme" in t:
            return "1"
        if (
            t.startswith("εσύ")
            or t.startswith("εσυ")
            or t.startswith("εσε")
            or t.startswith("esy")
            or t.startswith("ese")
            or "εσεί" in t
            or t.startswith("εσει")
        ):
            return "2"
        if t.startswith("αυτ") or t.startswith("aut"):
            return "3"
        return ""

    def person_number_slot(cell: str) -> tuple[str, str]:
        """Return (person '1'|'2'|'3', number 'sg'|'pl'|'')."""
        t = strip_style_markers(cell or "").strip().lower()
        person = person_number(cell)
        number = ""
        if re.search(r"\b(pl|plur|plural)\b", t) or any(
            x in t
            for x in (
                "εμείς",
                "εμεις",
                "εσείς",
                "εσεις",
                "αυτοί",
                "αυτοι",
                "αυτές",
                "αυτες",
                "αυτά",
                "αυτα",
                "emeis",
                "eseis",
            )
        ):
            number = "pl"
        elif re.search(r"\b(sg|sing|singular)\b", t) or person:
            # pronoun defaults: εγώ/εσύ/αυτός = sg unless plural form matched above
            if person and number != "pl":
                number = "sg"
        return person, number

    # | Person | Form | (2 cols, often 6 pronoun rows) → classic | | Sg | Pl |
    if max(len(r) for r in rows) == 2:
        grid = {"1": {"sg": "", "pl": ""}, "2": {"sg": "", "pl": ""}, "3": {"sg": "", "pl": ""}}
        filled = False
        for r in body:
            person, number = person_number_slot(r[0] if r else "")
            form = strip_style_markers(r[1] if len(r) > 1 else "").strip()
            if person and number and form:
                grid[person][number] = form
                filled = True
        if filled:
            return [
                ["", "Sg", "Pl"],
                ["1", grid["1"]["sg"], grid["1"]["pl"]],
                ["2", grid["2"]["sg"], grid["2"]["pl"]],
                ["3", grid["3"]["sg"], grid["3"]["pl"]],
            ]
        # Fallback: sequential 1..n + form (no pronouns).
        out = [["", "Form"]]
        for index, r in enumerate(body, start=1):
            form = strip_style_markers(r[1] if len(r) > 1 else "").strip()
            out.append([str(index), form])
        return out

    # | Person | Sg | Pl | … → empty corner + short 1/2/3 labels
    new_header = [""] + [strip_style_markers(c or "").strip() for c in rows[0][1:]]
    for i, c in enumerate(list(new_header)):
        cl = c.lower()
        if cl in ("singular", "sing.", "sing"):
            new_header[i] = "Sg"
        elif cl in ("plural", "plur.", "plur"):
            new_header[i] = "Pl"
    out = [new_header]
    for r in body:
        cells = list(r) + [""] * (len(new_header) - len(r) + 1)
        label = person_number(cells[0])
        out.append(
            [label]
            + [strip_style_markers(c or "").strip() for c in cells[1 : len(new_header)]]
        )
    return out


def _normalize_rows(rows: List[List[str]]) -> List[List[str]]:
    if not rows:
        return []
    ncols = max((len(r) for r in rows), default=0)
    if ncols == 0:
        return []
    normalized = [list(r) + [""] * (ncols - len(r)) for r in rows]
    # Drop empty trailing columns.
    while ncols > 1 and all(not (r[ncols - 1] or "").strip() for r in normalized):
        ncols -= 1
        normalized = [r[:ncols] for r in normalized]
    # Drop fully empty rows (except keep a single empty edge case as nothing).
    normalized = [r for r in normalized if any((c or "").strip() for c in r)]
    # Conjugation tables: no Person/pronoun column on the e-ink.
    normalized = _strip_person_column(normalized)
    # Re-normalize widths after a possible column drop.
    if normalized:
        ncols = max(len(r) for r in normalized)
        normalized = [list(r) + [""] * (ncols - len(r)) for r in normalized]
    return normalized


def _forward_fill_rows(rows: List[List[str]]) -> List[List[str]]:
    """Fill empty leading cells from the previous row (rowspan-style grammar tables)."""

    if not rows:
        return rows
    filled: List[List[str]] = []
    prev: List[str] = []
    for row in rows:
        out = list(row)
        if prev:
            for index, cell in enumerate(out):
                if (cell or "").strip():
                    break
                if index < len(prev) and (prev[index] or "").strip():
                    out[index] = prev[index]
        filled.append(out)
        prev = out
    return filled


def _format_table_stacked(rows: List[List[str]], width_budget: int = DEFAULT_WIDTH_BUDGET) -> str:
    """Wide tables: label + 'Header: value' lines (never silent crop).

    Empty first cells keep the previous section (no forward-fill labels).
    Long 'Key: value' lines split so groß/portrait does not wrap mid-token.
    """

    if not rows:
        return ""
    header = rows[0]
    body = rows[1:] if len(rows) > 1 else rows
    use_header = len(rows) > 1
    lines: List[str] = []
    # Leave room for 2-space indent on detail lines.
    detail_budget = max(12, width_budget - 2)

    def add_detail(key: str, cell: str) -> None:
        if key and cell:
            one = f"  {key}: {cell}"
            if len(one) <= width_budget:
                lines.append(one)
            else:
                lines.append(f"  {key}:")
                # Value on next line; soft-break only on spaces/commas.
                val = cell
                while val:
                    if len(val) <= detail_budget:
                        lines.append(f"  {val}")
                        break
                    window = val[: detail_budget + 1]
                    br = max(window.rfind(" "), window.rfind(","))
                    if br <= 0:
                        lines.append(f"  {val[:detail_budget]}")
                        val = val[detail_budget:].lstrip(" ")
                    else:
                        chunk = val[: br + (1 if val[br] == "," else 0)].rstrip()
                        lines.append(f"  {chunk}")
                        val = val[br + 1 :].lstrip(" ")
        elif key:
            lines.append(f"  {key}:")
        elif cell:
            lines.append(f"  {cell}")

    for row in body:
        label = (row[0] or "").strip()
        if label:
            lines.append(label if label.endswith(".") else label)
        start = 1 if use_header or label else 0
        for index in range(start, len(row)):
            cell = (row[index] or "").strip()
            if not cell and use_header:
                continue
            if use_header and index < len(header) and (header[index] or "").strip():
                add_detail(header[index].strip(), cell)
            elif cell:
                add_detail("", cell)
    return "\n".join(lines)


def _format_table_compact(rows: List[List[str]], width_budget: int) -> str:
    """Readable multi-column rows without monospace box alignment.

    Prefers short space-separated lines (fit groß/portrait). Falls back to
    ' · ' separators only when every line still fits the budget.

    Example (forward-filled gender)::

        Masc. Sing. ὁ -ος, -ης, -ευς
        Masc. Plur. οἱ -οι, -ες
    """

    if not rows:
        return ""
    header = rows[0]
    body = rows[1:] if len(rows) > 1 else []
    use_header = bool(body)
    data = body if use_header else rows
    if data:
        data = _forward_fill_rows(data)

    def parts_of(row: Sequence[str]) -> List[str]:
        parts = [(c or "").strip() for c in row]
        while parts and not parts[-1]:
            parts.pop()
        return parts

    def join_spaced(row: Sequence[str]) -> str:
        return " ".join(parts_of(row))

    def join_dot(row: Sequence[str]) -> str:
        return " · ".join(parts_of(row))

    def join_spaced_tight_commas(row: Sequence[str]) -> str:
        """Like space-join but compress ', ' → ',' in cells (saves cols on endings)."""
        parts = [p.replace(", ", ",") for p in parts_of(row)]
        return " ".join(parts)

    def split_last_col(row: Sequence[str]) -> List[str]:
        """Lead columns on line 1, last column indented on line 2 (groß-safe)."""
        parts = parts_of(row)
        if len(parts) < 2:
            return [" ".join(parts)] if parts else []
        head = " ".join(parts[:-1])
        tail = parts[-1]
        return [head, f"  {tail}"]

    # Try densest forms first. Long headers are optional (omit when over budget).
    candidates: List[List[str]] = []
    for joiner in (join_spaced_tight_commas, join_spaced, join_dot):
        data_lines = [joiner(row) for row in data]
        if use_header:
            head = joiner(header)
            if len(head) <= width_budget:
                candidates.append([head] + data_lines)
            candidates.append(data_lines)
        else:
            candidates.append(data_lines)

    # Two-line rows (last column alone) — still compact, fits 2× portrait.
    split_lines: List[str] = []
    for row in data:
        split_lines.extend(split_last_col(row))
    candidates.append(split_lines)

    def max_len(lines: List[str]) -> int:
        return max((len(line) for line in lines), default=0)

    for lines in candidates:
        if lines and max_len(lines) <= width_budget:
            return "\n".join(lines)
    for lines in candidates:
        if lines and max_len(lines) <= width_budget + 2:
            return "\n".join(lines)
    return ""


def _pad_cell(cell: str, width: int) -> str:
    """Right-pad cell to *width* display columns (bold markers = zero width)."""

    cell = cell or ""
    pad = width - _display_width(cell)
    if pad > 0:
        return cell + (" " * pad)
    if pad < 0:
        # Truncate visible run; keep style markers intact by stripping for cut.
        visible = strip_style_markers(cell)
        cut = max(1, width - 1)
        # Prefer simple prefix when no markers.
        if cell == visible:
            return visible[:cut] + "…"
        # With markers: clamp to width on visible text and re-wrap loosely.
        return _clamp(visible, width)
    return cell


def _table_line_width(widths: Sequence[int], pad: int = 1) -> int:
    """Outer width of an ASCII box row: | p cell p | … |"""

    ncols = len(widths)
    if ncols == 0:
        return 0
    # each cell: pad + content + pad; plus (ncols + 1) vertical bars
    return sum(w + 2 * pad for w in widths) + (ncols + 1)


def _conjugation_body_rows(rows: List[List[str]]) -> List[List[str]] | None:
    """If *rows* is a 1/2/3 paradigm, return body only (no Sg/Pl header).

    Conjugation tables on the e-ink omit the header row and all ``+---+`` /
    ``|---|`` rule lines — only the person rows remain.
    """

    if not rows:
        return None

    def cell0(row: Sequence[str]) -> str:
        return strip_style_markers((row[0] if row else "") or "").strip()

    def is_person_label(lab: str) -> bool:
        return lab in ("1", "2", "3")

    def header_looks_number_or_form(header: Sequence[str]) -> bool:
        cells = [strip_style_markers((c or "")).strip().lower() for c in header]
        if any(
            h in ("sg", "pl", "singular", "plural", "form", "forms", "sing", "plur")
            for h in cells
        ):
            return True
        # empty corner + column labels (e.g. "", "Aorist", …) with person body
        if cells and not cells[0]:
            return True
        if cells and cells[0] in ("person", "pers", "p", "pers."):
            return True
        return False

    if len(rows) >= 2 and header_looks_number_or_form(rows[0]):
        body = rows[1:]
        labels = [cell0(r) for r in body]
        person_n = sum(1 for lab in labels if is_person_label(lab))
        if person_n >= 2 and person_n >= max(2, (len(labels) + 1) // 2):
            return body

    # Already body-only: rows labelled 1/2/3 with no Sg/Pl header.
    labels = [cell0(r) for r in rows]
    person_n = sum(1 for lab in labels if is_person_label(lab))
    if (
        person_n >= 2
        and person_n == len(labels)
        and max(len(r) for r in rows) >= 2
    ):
        return list(rows)
    return None


def _format_table_box(
    rows: List[List[str]],
    widths: List[int],
    *,
    cell_pad: int = 1,
    rules: bool = True,
) -> str:
    """ASCII box table (legacy / debug — UI_12 has no Unicode box-drawing glyphs).

    Prefer :func:`_emit_vector_table` for the X4 wire format. Kept for tests and
    as a readable fallback when vector emission is disabled.
    """

    def rule() -> str:
        parts = ["+"]
        for width in widths:
            parts.append("-" * (width + 2 * cell_pad))
            parts.append("+")
        return "".join(parts)

    def data_row(row: Sequence[str]) -> str:
        parts = ["|"]
        for index, width in enumerate(widths):
            cell = _pad_cell(row[index] if index < len(row) else "", width)
            pad = " " * cell_pad
            parts.append(pad)
            parts.append(cell)
            parts.append(pad)
            parts.append("|")
        return "".join(parts)

    if not rules:
        return "\n".join(data_row(row) for row in rows)

    lines: List[str] = [rule(), data_row(rows[0]), rule()]
    for row in rows[1:]:
        lines.append(data_row(row))
    if len(rows) > 1:
        lines.append(rule())
    return "\n".join(lines)


def _emit_vector_table(rows: List[List[str]], flags: str = "") -> str:
    """Emit an X4 vector-table block (device draws 1px grid, O(cols) RAM).

    Cells are single-line; bold STX/ETX markers inside cells are preserved.
    """

    if not rows:
        return ""
    cols = max((len(row) for row in rows), default=0)
    if cols < 1:
        return ""
    cols = min(cols, MAX_TABLE_COLUMNS)
    nrows = min(len(rows), MAX_TABLE_ROWS)
    flag = "".join(ch for ch in (flags or "") if ch in ("C", "H", "B"))
    lines: List[str] = [f"{TABLE_RS}table c={cols} r={nrows} f={flag}"]
    for row in rows[:nrows]:
        cells: List[str] = []
        for index in range(cols):
            cell = row[index] if index < len(row) else ""
            cell = (cell or "").replace("\r", " ").replace("\n", " ")
            cell = cell.replace(CELL_US, " ").replace(TABLE_RS, "")
            cells.append(cell)
        lines.append(CELL_US.join(cells))
    lines.append(TABLE_END_LINE)
    return "\n".join(lines)


def _clean_fig_cell(cell: str) -> str:
    cell = (cell or "").replace("\r", " ").replace("\n", " ")
    cell = cell.replace(CELL_US, " ").replace(TABLE_RS, "")
    return cell.strip()


def emit_fig_stem(stem: str, ending: str, result: str = "") -> str:
    """Stem + ending > result boxes (X4 vector fig)."""
    a, b, c = _clean_fig_cell(stem), _clean_fig_cell(ending), _clean_fig_cell(result)
    if not a and not b:
        return ""
    if not c and a and b:
        c = f"{a}{b}"
    body = CELL_US.join([a, b, c])
    return f"{TABLE_RS}fig t=stem\n{body}\n{FIG_END_LINE}"


def emit_fig_stress(syllables: Sequence[str], stress_index: int = 0) -> str:
    """Syllable boxes with stress mark on stress_index (0-based)."""
    cells = [_clean_fig_cell(s) for s in syllables if _clean_fig_cell(s)]
    if not cells:
        return ""
    cells = cells[:MAX_FIG_CELLS]
    idx = max(0, min(int(stress_index), len(cells) - 1))
    body = CELL_US.join(cells)
    return f"{TABLE_RS}fig t=stress s={idx}\n{body}\n{FIG_END_LINE}"


def emit_fig_timeline(
    labels: Sequence[str],
    marker_index: int = 0,
    event: str = "",
) -> str:
    """Horizontal timeline with tick labels and optional event above the marker."""
    cells = [_clean_fig_cell(s) for s in labels if _clean_fig_cell(s)]
    if len(cells) < 2:
        return ""
    cells = cells[:MAX_FIG_CELLS]
    idx = max(0, min(int(marker_index), len(cells) - 1))
    lines = [
        f"{TABLE_RS}fig t=timeline m={idx}",
        CELL_US.join(cells),
    ]
    ev = _clean_fig_cell(event)
    if ev:
        lines.append(ev)
    lines.append(FIG_END_LINE)
    return "\n".join(lines)


# Authoring shortcuts → vector figs (converted on pull).
#   [fig:stem|λυ-|-ω|λύω]
#   [fig:stress:2|κα|λη|μέ|ρα]
#   [fig:timeline:1|Verg.|Jetzt|Zukunft|Aorist?]
_FIG_BRACKET_RE = re.compile(
    r"\[fig:(stem|stress|timeline)(?::(\d+))?\|([^\]]+)\]",
    flags=re.IGNORECASE,
)
# Fenced:
#   ```fig stem
#   λυ- | -ω | λύω
#   ```
_FIG_FENCE_RE = re.compile(
    r"```fig[ \t]+(stem|stress|timeline)(?:[ \t]+(\d+))?[ \t]*\n(.*?)```",
    flags=re.IGNORECASE | re.DOTALL,
)


def _parts_from_fig_body(body: str) -> List[str]:
    raw = (body or "").strip()
    if "|" in raw:
        return [_clean_fig_cell(p) for p in raw.split("|") if _clean_fig_cell(p)]
    # whitespace / newlines
    parts: List[str] = []
    for line in raw.replace("\r", "\n").split("\n"):
        line = line.strip()
        if not line:
            continue
        if "|" in line:
            parts.extend(_clean_fig_cell(p) for p in line.split("|") if _clean_fig_cell(p))
        else:
            parts.append(_clean_fig_cell(line))
    return [p for p in parts if p]


def _fig_from_type(kind: str, index: int | None, parts: List[str]) -> str:
    kind = (kind or "").lower()
    if kind == "stem":
        while len(parts) < 3:
            parts.append("")
        return emit_fig_stem(parts[0], parts[1], parts[2] if len(parts) > 2 else "")
    if kind == "stress":
        idx = 0 if index is None else index
        return emit_fig_stress(parts, idx)
    if kind == "timeline":
        idx = 0 if index is None else index
        # last part may be event label if more labels than needed — if 4+ parts,
        # treat last as event when index was given and len>3.
        event = ""
        labels = parts
        if len(parts) >= 4:
            event = parts[-1]
            labels = parts[:-1]
        return emit_fig_timeline(labels, idx, event)
    return ""


def expand_fig_shortcuts(text: str) -> str:
    """Turn [fig:…] and ```fig …``` authoring into \\x04fig wire blocks."""
    if not text:
        return text

    def repl_bracket(m: re.Match[str]) -> str:
        kind = m.group(1)
        idx = int(m.group(2)) if m.group(2) is not None else None
        parts = _parts_from_fig_body(m.group(3))
        return _fig_from_type(kind, idx, parts) or m.group(0)

    def repl_fence(m: re.Match[str]) -> str:
        kind = m.group(1)
        idx = int(m.group(2)) if m.group(2) is not None else None
        parts = _parts_from_fig_body(m.group(3))
        # stress/timeline: first number may also be first line
        return _fig_from_type(kind, idx, parts) or m.group(0)

    text = _FIG_FENCE_RE.sub(repl_fence, text)
    text = _FIG_BRACKET_RE.sub(repl_bracket, text)
    return text


def _cells_are_single_line(rows: Sequence[Sequence[str]]) -> bool:
    for row in rows:
        for cell in row:
            if cell and ("\n" in cell or "\r" in cell):
                return False
    return True


def _shrink_widths_to_budget(
    widths: List[int],
    width_budget: int,
    *,
    cell_pad: int,
    min_widths: Sequence[int],
) -> List[int] | None:
    """Shrink columns until the box fits, never below per-column min_widths."""

    widths = list(widths)
    mins = list(min_widths)
    if len(mins) < len(widths):
        mins = mins + [1] * (len(widths) - len(mins))
    total = _table_line_width(widths, pad=cell_pad)
    if total <= width_budget:
        return widths
    overflow = total - width_budget
    while overflow > 0:
        order = sorted(range(len(widths)), key=lambda i: widths[i], reverse=True)
        progressed = False
        for index in order:
            floor = max(1, mins[index])
            if widths[index] <= floor:
                continue
            widths[index] -= 1
            overflow -= 1
            progressed = True
            if overflow <= 0:
                break
        if not progressed:
            return None
    return widths


def format_table(
    rows: List[List[str]],
    width_budget: int = DEFAULT_WIDTH_BUDGET,
    max_columns: int = MAX_TABLE_COLUMNS,
) -> str:
    """Layout a table as a vector block, compact rows, or stacked key/value lines.

    When the grid fits the width budget (2–4 cols, ≤12 single-line rows), emit an
    X4 **vector table** block (``\\x04table …``). The device measures cells in
    pixels and draws 1px rules — no ASCII ``+---+`` chrome on the wire.

    Wide tables become compact or stacked short lines so the device never wraps
    mid-row (wrap destroys table structure).

    Conjugation paradigms (1/2/3 × Sg|Pl) drop the header row; flag ``C`` omits
    the outer box on the device.
    """

    normalized = _normalize_rows(rows)
    if not normalized:
        return ""

    # Resolve **bold** / HTML emphasis before measuring columns so padding
    # matches what the device draws (markers are zero-width).
    normalized = [
        [_apply_inline_styles(cell) if cell else "" for cell in row]
        for row in normalized
    ]

    conj_body = _conjugation_body_rows(normalized)
    # Conjugation: render body only (no Sg/Pl header).
    display = conj_body if conj_body is not None else normalized
    is_conjugation = conj_body is not None

    ncols = len(display[0])
    if ncols > max_columns or len(display) > MAX_TABLE_ROWS:
        return _format_table_stacked(
            normalized if not is_conjugation else display, width_budget=width_budget
        )

    if not _cells_are_single_line(display):
        return _format_table_stacked(
            normalized if not is_conjugation else display, width_budget=width_budget
        )

    natural: List[int] = [0] * ncols
    for row in display:
        for index, cell in enumerate(row):
            natural[index] = max(natural[index], _display_width(cell or ""))

    # Ensure at least 1 column width for empty header corner cells.
    natural = [
        max(w, 1) if any(r[i] for r in display) else max(w, 0)
        for i, w in enumerate(natural)
    ]
    for index, width in enumerate(natural):
        if width == 0:
            natural[index] = 1

    # Vector grid when cells fit without aggressive truncation (same budget as
    # the old padded ASCII box). Mild shrink: long cols may lose ≤2 glyphs.
    mild_mins = [max(1, w - 2) if w > 6 else w for w in natural]
    widths = _shrink_widths_to_budget(
        natural, width_budget, cell_pad=1, min_widths=mild_mins
    )
    if widths is None and ncols <= 2:
        widths = _shrink_widths_to_budget(
            natural, width_budget, cell_pad=0, min_widths=mild_mins
        )
        if widths is None:
            # 2-col last resort: still emit vector (device ellipsizes in px).
            widths = list(natural)

    if widths is not None:
        if is_conjugation:
            flags = "C"
        elif len(display) > 1:
            flags = "H"
        else:
            flags = "B"
        return _emit_vector_table(display, flags)

    # 3–4 columns that do not fit: compact short rows, else stacked.
    if is_conjugation:
        lines = [" ".join((c or "").strip() for c in row if (c or "").strip()) for row in display]
        lines = [ln for ln in lines if ln]
        if lines and max(len(ln) for ln in lines) <= width_budget:
            return "\n".join(lines)
        return _format_table_stacked(display, width_budget=width_budget)

    compact = _format_table_compact(normalized, width_budget=width_budget)
    if compact:
        return compact
    return _format_table_stacked(normalized, width_budget=width_budget)


def _parse_html_table(table_html: str) -> List[List[str]]:
    rows: List[List[str]] = []
    for tr_match in _TR_RE.finditer(table_html):
        cells = [_inline_to_text(cell) for cell in _CELL_RE.findall(tr_match.group(1))]
        if cells:
            rows.append(cells)
    return rows


def _replace_html_tables(text: str, width_budget: int) -> str:
    def repl(match: re.Match[str]) -> str:
        rows = _parse_html_table(match.group(0))
        laid = format_table(rows, width_budget=width_budget)
        return f"\n{laid}\n" if laid else "\n"

    return _TABLE_RE.sub(repl, text)


def _replace_html_lists(text: str) -> str:
    """Replace ul/ol blocks (innermost-first via repeated passes)."""

    for _ in range(12):

        def repl(match: re.Match[str]) -> str:
            tag = match.group(1).lower()
            body = match.group(2)
            items = _LI_RE.findall(body)
            if not items:
                return "\n"
            lines: List[str] = []
            for index, item in enumerate(items, start=1):
                # Nested lists already replaced in prior passes may leave lines.
                content = _inline_to_text(item)
                if not content:
                    # Preserve pre-converted nested bullet lines inside li.
                    inner = _preprocess_media(item)
                    inner = _TAG_RE.sub("\n", inner)
                    inner = html.unescape(inner)
                    content = _compact_blank_lines(inner)
                if tag == "ol":
                    lines.append(f"{index}. {content}" if content else f"{index}.")
                else:
                    lines.append(f"• {content}" if content else "•")
            return "\n" + "\n".join(lines) + "\n"

        new_text = _LIST_RE.sub(repl, text)
        if new_text == text:
            break
        text = new_text
    return text


def _replace_html_blockquotes(text: str) -> str:
    def repl(match: re.Match[str]) -> str:
        inner = _inline_to_text(match.group(1))
        if not inner:
            return "\n│\n"
        parts = [p.strip() for p in re.split(r"\n+", inner) if p.strip()]
        if not parts:
            parts = [inner]
        return "\n" + "\n".join(f"│ {p}" for p in parts) + "\n"

    return _BLOCKQUOTE_RE.sub(repl, text)


def _replace_html_headings(text: str) -> str:
    def repl(match: re.Match[str]) -> str:
        title = _inline_to_text(match.group(2))
        return f"\n{title}\n" if title else "\n"

    text = _H12_RE.sub(repl, text)
    text = _H36_RE.sub(repl, text)
    return text


def _is_md_separator_row(line: str) -> bool:
    """True for GFM table separator rows like `| --- | :---: |`."""

    stripped = line.strip()
    if not stripped:
        return False
    cells = _split_md_row(stripped)
    non_empty = [c for c in cells if c]
    if not non_empty:
        return False
    return all(_MD_SEP_CELL_RE.match(c.replace(" ", "")) for c in non_empty)


def _split_md_row(line: str) -> List[str]:
    raw = line.strip()
    if raw.startswith("|"):
        raw = raw[1:]
    if raw.endswith("|"):
        raw = raw[:-1]
    return [cell.strip() for cell in raw.split("|")]


def _looks_like_md_table(lines: Sequence[str], index: int) -> bool:
    if index + 1 >= len(lines):
        return False
    header = lines[index].strip()
    sep = lines[index + 1].strip()
    if "|" not in header:
        return False
    if not _is_md_separator_row(sep):
        return False
    # Separator should have roughly as many columns as header.
    return len(_split_md_row(sep)) >= 1


def _consume_md_table(
    lines: Sequence[str], start: int
) -> Tuple[List[List[str]], int]:
    rows = [_split_md_row(lines[start])]
    index = start + 2  # skip header + separator
    while index < len(lines):
        line = lines[index].strip()
        if not line or "|" not in line:
            break
        if _is_md_separator_row(line):
            break
        # Stop if line looks like a list/heading rather than a table row.
        if _MD_HEADING_RE.match(line) or _MD_UL_RE.match(line) or (
            _MD_OL_RE.match(line) and line.count("|") == 0
        ):
            break
        rows.append(_split_md_row(line))
        index += 1
    return rows, index


def _markdown_blocks(text: str, width_budget: int) -> str:
    lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    out: List[str] = []
    index = 0
    while index < len(lines):
        raw = lines[index]
        stripped = raw.strip()

        if _looks_like_md_table(lines, index):
            table_rows, next_index = _consume_md_table(lines, index)
            laid = format_table(table_rows, width_budget=width_budget)
            if laid:
                out.append(laid)
            index = next_index
            continue

        if not stripped:
            out.append("")
            index += 1
            continue

        heading = _MD_HEADING_RE.match(stripped)
        if heading:
            out.append(heading.group(2).strip())
            index += 1
            continue

        if _MD_HR_RE.match(stripped):
            out.append("────")
            index += 1
            continue

        if stripped.startswith(">"):
            quote = re.sub(r"^>\s?", "", stripped)
            out.append(f"│ {quote}" if quote else "│")
            index += 1
            continue

        unordered = _MD_UL_RE.match(stripped)
        if unordered:
            out.append(f"• {unordered.group(2)}")
            index += 1
            continue

        ordered = _MD_OL_RE.match(stripped)
        if ordered:
            out.append(f"{ordered.group(1)}. {ordered.group(2)}")
            index += 1
            continue

        out.append(raw)
        index += 1

    return "\n".join(out)


def to_device_text(
    source: str,
    limit: int = 4096,
    width_budget: int = DEFAULT_WIDTH_BUDGET,
) -> str:
    """Convert Anki HTML and/or XFD Markdown to e-ink plain text."""

    if not source:
        return ""

    text = str(source)
    text = _preprocess_media(text)
    # Authoring shortcuts → vector figs before HTML/table layout.
    text = expand_fig_shortcuts(text)
    # Compact 3rd-person pronoun triples before layout (shorter cells/lines).
    text = normalize_pronoun_short_forms(text)
    # Bold tags before table/list extraction so cells keep emphasis.
    text = _html_bold_tags_to_markers(text)
    text = _replace_html_tables(text, width_budget=width_budget)
    text = _replace_html_lists(text)
    text = _replace_html_blockquotes(text)
    text = _replace_html_headings(text)
    text = _HR_RE.sub("\n────\n", text)
    text = _BR_RE.sub("\n", text)
    text = _BLOCK_OPEN_CLOSE_RE.sub("\n", text)
    # Leftover structural tags → line breaks; then drop remaining tags.
    text = _STRUCTURAL_TAG_RE.sub("\n", text)
    text = _TAG_RE.sub(" ", text)
    text = html.unescape(text).replace("\r\n", "\n").replace("\xa0", " ")

    text = _markdown_blocks(text, width_budget=width_budget)
    # Markdown bold + code after block parse (lists/headings already handled).
    text = _md_bold_to_markers(text)
    text = _CODE_RE.sub(r"\1", text)
    text = normalize_pronoun_short_forms(text)
    # UI_12/UI_18 have no Unicode arrows / fancy dashes — rewrite before e-ink draw.
    text = _normalize_device_punctuation(text)
    text = _compact_blank_lines(text)
    # Memory line (※ …) must not restate the lemma already shown above it.
    text = strip_memory_lemma_echo(text)
    return _clamp(text, limit)


def plain_text(card_html: str, limit: int = 4096) -> str:
    """Convert Anki card/field HTML to compact plain text for the e-ink device.

    Uses the XFD converter (tables, lists, headings, …). Kept as the stable
    public name used by the add-on and tests.
    """

    return to_device_text(card_html, limit=limit)


def answer_only(answer_html: str) -> str:
    """Return the back side after Anki's answer separator, if present."""

    if not answer_html:
        return ""
    for pattern in (_ANSWER_SEPARATOR_RE, _ANSWER_SEPARATOR_LOOSE_RE):
        split = pattern.split(answer_html, maxsplit=1)
        # Capturing groups from re.split produce extra list entries.
        if len(split) >= 2:
            return split[-1]
    return answer_html


def _first_named(fields: Dict[str, str], names: Sequence[str]) -> str:
    lower_map = {key.lower(): value for key, value in fields.items()}
    for name in names:
        value = lower_map.get(name.lower())
        if value:
            return value
    return ""


def field_map_from_pairs(pairs: Iterable[Tuple[str, str]], limit: int = 4096) -> Dict[str, str]:
    result: Dict[str, str] = {}
    for name, raw in pairs:
        plain = to_device_text(raw or "", limit=limit)
        if plain:
            result[str(name)] = plain
    return result


def sides_from_fields(
    fields: Dict[str, str],
    text_limit: int = 4096,
    reverse: bool = False,
) -> Tuple[str, str]:
    """Pick front/back from named or ordered note fields."""

    if not fields:
        return "", ""

    front = _first_named(fields, _FRONT_FIELD_NAMES)
    back = _first_named(fields, _BACK_FIELD_NAMES)
    values = list(fields.values())

    if not front:
        front = values[0]
    if not back:
        # Prefer a field that differs from the front.
        back = next((value for value in values if value != front), values[-1])

    if reverse:
        front, back = back, front

    return front[:text_limit], back[:text_limit]


def dump_fields(fields: Dict[str, str], text_limit: int = 4096) -> str:
    if not fields:
        return ""
    lines = [f"{name}: {value}" for name, value in fields.items()]
    return "\n".join(lines)[:text_limit]


def combine_sides(
    rendered_front: str,
    rendered_back: str,
    fields: Dict[str, str],
    text_limit: int = 4096,
    reverse: bool = False,
) -> Tuple[str, str]:
    """Prefer rendered template text; fall back to note fields; never leave blank."""

    front = (rendered_front or "").strip()
    back = (rendered_back or "").strip()
    field_front, field_back = sides_from_fields(fields, text_limit, reverse=reverse)

    if not front:
        front = field_front
    if not back:
        back = field_back

    # If rendering produced only whitespace/media markers but fields have real words,
    # prefer fields when the rendered side is a weak placeholder.
    weak = {"", "[…]", "[Bild]", "[bild]"}
    if front in weak and field_front:
        front = field_front
    if back in weak and field_back:
        back = field_back

    if not front or not back:
        dumped = dump_fields(fields, text_limit)
        if not front:
            front = dumped
        if not back:
            back = dumped

    front = front.strip() or EMPTY_SIDE_PLACEHOLDER
    back = back.strip() or EMPTY_SIDE_PLACEHOLDER
    if len(front) > text_limit:
        front = front[: text_limit - 1].rstrip() + "…"
    if len(back) > text_limit:
        back = back[: text_limit - 1].rstrip() + "…"
    return front, back


def looks_reversed_template(template_name: str = "", note_type_name: str = "", ord_: int = 0) -> bool:
    tmpl = (template_name or "").lower()
    ntype = (note_type_name or "").lower()
    if "reverse" in tmpl or "umgekehrt" in tmpl or "rück" in tmpl:
        return True
    if ord_ == 1 and ("basic" in ntype or "basis" in ntype):
        return True
    return False
