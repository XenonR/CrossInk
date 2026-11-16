import importlib.util
import pathlib
import sys
import unittest


TEXTUTIL_PATH = pathlib.Path(__file__).parents[1] / "xteink_sync" / "textutil.py"
SPEC = importlib.util.spec_from_file_location("xteink_textutil", TEXTUTIL_PATH)
assert SPEC and SPEC.loader
TEXTUTIL = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = TEXTUTIL
SPEC.loader.exec_module(TEXTUTIL)


class TextUtilTests(unittest.TestCase):
    def test_fig_stem_emit_and_shortcut(self):
        block = TEXTUTIL.emit_fig_stem("λυ-", "-ω", "λύω")
        self.assertIn(TEXTUTIL.FIG_START_PREFIX or "\x04fig ", block)
        self.assertIn("t=stem", block)
        self.assertIn("λυ-", block)
        self.assertTrue(block.endswith(TEXTUTIL.FIG_END_LINE) or "\x04/fig" in block)
        short = TEXTUTIL.plain_text("[fig:stem|λυ-|-ω|λύω]")
        self.assertIn("t=stem", short)
        self.assertIn("\x06", short)

    def test_fig_stress_and_timeline_shortcuts(self):
        s = TEXTUTIL.plain_text("[fig:stress:2|κα|λη|μέ|ρα]")
        self.assertIn("t=stress", s)
        self.assertIn("s=2", s)
        t = TEXTUTIL.plain_text("[fig:timeline:1|Verg.|Jetzt|Zukunft|Aorist?]")
        self.assertIn("t=timeline", t)
        self.assertIn("m=1", t)
        self.assertIn("Aorist?", t)

    def test_fig_fence(self):
        md = "```fig stem\nλυ- | -ω | λύω\n```"
        out = TEXTUTIL.plain_text(md)
        self.assertIn("t=stem", out)
        self.assertIn("λύω", out)

    def test_memory_line_strips_lemma_echo(self):
        raw = (
            f"{TEXTUTIL.BOLD_ON}πίνω{TEXTUTIL.BOLD_OFF}  (píno)\n"
            "\n"
            "※ πίνω ~ potion\n"
        )
        out = TEXTUTIL.strip_memory_lemma_echo(raw)
        self.assertIn("※ ~ potion", out)
        self.assertNotIn("※ πίνω", out)
        # End-to-end through plain_text
        html = "<b>παγωτό</b>  (pagotó)<br><br>※ παγώνω 'freeze' &gt; παγωτό 'ice cream'"
        plain = TEXTUTIL.plain_text(html)
        self.assertIn("※", plain)
        self.assertIn("παγώνω", plain)
        self.assertNotIn("※ παγωτό", plain)
        # Stem fragment must stay
        keep = TEXTUTIL.strip_memory_lemma_echo(
            f"{TEXTUTIL.BOLD_ON}ακριβώς{TEXTUTIL.BOLD_OFF}\n\n※ ακριβ- ~ akribisch\n"
        )
        self.assertIn("ακριβ- ~ akribisch", keep)

    def test_img_alt_becomes_text(self):
        text = TEXTUTIL.plain_text('<img src="haus.jpg" alt="Haus">')
        self.assertEqual(text, "[Haus]")

    def test_img_without_alt_is_marker(self):
        text = TEXTUTIL.plain_text('<div><img src="x.png"></div>')
        self.assertEqual(text, "[Bild]")

    def test_cloze_field_keeps_answer(self):
        text = TEXTUTIL.plain_text("Die {{c1::Wahrheit}} zählt.")
        self.assertEqual(text, "Die Wahrheit zählt.")

    def test_answer_separator_split(self):
        html = "<div>front</div><hr id=answer><div>back side</div>"
        only = TEXTUTIL.answer_only(html)
        self.assertIn("back side", only)
        self.assertNotIn("front", TEXTUTIL.plain_text(only))

    def test_fields_fallback_named(self):
        fields = {"Wort": "ἀλήθεια", "Übersetzung": "Wahrheit"}
        front, back = TEXTUTIL.combine_sides("", "", fields)
        self.assertEqual(front, "ἀλήθεια")
        self.assertEqual(back, "Wahrheit")

    def test_reverse_swaps_named_fields(self):
        fields = {"Front": "A", "Back": "B"}
        front, back = TEXTUTIL.combine_sides("", "", fields, reverse=True)
        self.assertEqual(front, "B")
        self.assertEqual(back, "A")

    def test_never_blank(self):
        front, back = TEXTUTIL.combine_sides("", "", {})
        self.assertEqual(front, TEXTUTIL.EMPTY_SIDE_PLACEHOLDER)
        self.assertEqual(back, TEXTUTIL.EMPTY_SIDE_PLACEHOLDER)

    def test_rendered_preferred_over_fields(self):
        fields = {"Front": "field-front", "Back": "field-back"}
        front, back = TEXTUTIL.combine_sides("rendered Q", "rendered A", fields)
        self.assertEqual(front, "rendered Q")
        self.assertEqual(back, "rendered A")

    def test_greek_and_german_survive(self):
        text = TEXTUTIL.plain_text("<p>ἡ ἀλήθεια</p><p>die Wahrheit</p>")
        self.assertIn("ἀλήθεια", text)
        self.assertIn("Wahrheit", text)


class XfdConverterTests(unittest.TestCase):
    def test_md_heading_list_bold_quote_hr(self):
        src = "\n".join(
            [
                "## Aorist Passiv",
                "",
                "- Kennzeichen **-θη-**",
                "1. Stamm wählen",
                "",
                "> Nach Dental oft Assimilation",
                "",
                "---",
                "",
                "Merke: `+ θη + Endung`",
            ]
        )
        text = TEXTUTIL.to_device_text(src)
        visible = TEXTUTIL.strip_style_markers(text)
        self.assertIn("Aorist Passiv", visible)
        self.assertNotIn("##", text)
        self.assertIn("• Kennzeichen ", text)
        self.assertIn(f"{TEXTUTIL.BOLD_ON}-θη-{TEXTUTIL.BOLD_OFF}", text)
        self.assertIn("1. Stamm wählen", text)
        self.assertIn("│ Nach Dental oft Assimilation", text)
        self.assertIn("────", text)
        self.assertIn("+ θη + Endung", text)
        self.assertNotIn("**", text)
        self.assertNotIn("`", text)

    def test_bold_markers_from_markdown_and_html(self):
        md = TEXTUTIL.to_device_text("Lemma **λύω** hier")
        self.assertEqual(md, f"Lemma {TEXTUTIL.BOLD_ON}λύω{TEXTUTIL.BOLD_OFF} hier")

        html = TEXTUTIL.to_device_text("Lemma <b>λύω</b> hier")
        self.assertEqual(html, f"Lemma {TEXTUTIL.BOLD_ON}λύω{TEXTUTIL.BOLD_OFF} hier")

        strong = TEXTUTIL.to_device_text("<strong>Endung</strong>")
        self.assertEqual(strong, f"{TEXTUTIL.BOLD_ON}Endung{TEXTUTIL.BOLD_OFF}")

    def test_bold_in_table_cell(self):
        src = "\n".join(
            [
                "| Form |",
                "| ---- |",
                "| **λύω** |",
            ]
        )
        text = TEXTUTIL.to_device_text(src)
        self.assertIn(f"{TEXTUTIL.BOLD_ON}λύω{TEXTUTIL.BOLD_OFF}", text)
        self.assertEqual(TEXTUTIL.strip_style_markers(text).count("λύω"), 1)
        # Vector table block; bold lives inside a cell row.
        self.assertIn(f"{TEXTUTIL.TABLE_RS}table ", text)
        self.assertIn(TEXTUTIL.TABLE_END_LINE, text)
        self.assertIn("Form", text)

    def test_table_columns_align_with_bold(self):
        src = "\n".join(
            [
                "|    | Sg     | Pl        |",
                "| -- | ------ | --------- |",
                "| 1  | **λύω** | λύομεν    |",
                "| 2  | λύεις  | λύετε     |",
            ]
        )
        text = TEXTUTIL.to_device_text(src)
        # Conjugation → vector block, flag C, no Sg/Pl header.
        self.assertIn(f"{TEXTUTIL.TABLE_RS}table ", text)
        self.assertIn("f=C", text)
        self.assertIn(TEXTUTIL.CELL_US, text)
        self.assertIn(f"{TEXTUTIL.BOLD_ON}λύω{TEXTUTIL.BOLD_OFF}", text)
        self.assertNotIn("Sg", TEXTUTIL.strip_style_markers(text))
        self.assertNotIn("Pl", TEXTUTIL.strip_style_markers(text))
        # Two person rows between start and end markers.
        body = [
            ln
            for ln in text.splitlines()
            if TEXTUTIL.CELL_US in ln or (ln and not ln.startswith(TEXTUTIL.TABLE_RS))
        ]
        # Filter to true cell rows only.
        cell_rows = [ln for ln in text.splitlines() if TEXTUTIL.CELL_US in ln]
        self.assertEqual(len(cell_rows), 2, msg=text)

    def test_md_pipe_table_paradigm(self):
        src = "\n".join(
            [
                "## λύω — Present Active",
                "",
                "|    | Sg     | Pl        |",
                "| -- | ------ | --------- |",
                "| 1  | λύω    | λύομεν    |",
                "| 2  | λύεις  | λύετε     |",
                "| 3  | λύει   | λύουσι(ν) |",
            ]
        )
        text = TEXTUTIL.to_device_text(src)
        self.assertIn("λύω — Present Active", text)
        self.assertIn("λύομεν", text)
        self.assertIn("λύουσι(ν)", text)
        # Conjugation: person rows only in a vector block — no Sg/Pl, no ASCII box.
        self.assertNotIn("Sg", text)
        self.assertNotIn("Pl", text)
        self.assertIn(f"{TEXTUTIL.TABLE_RS}table ", text)
        self.assertIn("f=C", text)
        self.assertIn(TEXTUTIL.TABLE_END_LINE, text)
        self.assertNotIn("+", text)
        cell_rows = [ln for ln in text.splitlines() if TEXTUTIL.CELL_US in ln]
        self.assertEqual(len(cell_rows), 3)
        self.assertIn("1", cell_rows[0])
        self.assertIn("3", cell_rows[2])

    def test_html_table(self):
        html = """
        <h2>εἰμί</h2>
        <table>
          <tr><th></th><th>Sg</th><th>Pl</th></tr>
          <tr><td>1</td><td>εἰμί</td><td>ἐσμέν</td></tr>
          <tr><td>2</td><td>εἶ</td><td>ἐστέ</td></tr>
        </table>
        """
        text = TEXTUTIL.to_device_text(html)
        self.assertIn("εἰμί", text)
        self.assertIn("ἐσμέν", text)
        self.assertNotIn("Sg", text)
        self.assertNotIn("+", text)
        self.assertIn(f"{TEXTUTIL.TABLE_RS}table ", text)
        self.assertIn("f=C", text)
        self.assertNotIn("<table", text.lower())

    def test_html_lists(self):
        html = "<ul><li>Alpha</li><li><b>Beta</b></li></ul><ol><li>Eins</li><li>Zwei</li></ol>"
        text = TEXTUTIL.to_device_text(html)
        self.assertIn("• Alpha", text)
        self.assertIn(f"• {TEXTUTIL.BOLD_ON}Beta{TEXTUTIL.BOLD_OFF}", text)
        self.assertIn("1. Eins", text)
        self.assertIn("2. Zwei", text)

    def test_html_blockquote(self):
        text = TEXTUTIL.to_device_text("<blockquote>Merksatz hier</blockquote>")
        self.assertEqual(text, "│ Merksatz hier")

    def test_wide_table_stacks(self):
        rows = [
            ["P", "A", "B", "C", "D", "E"],
            ["1", "a1", "b1", "c1", "d1", "e1"],
        ]
        laid = TEXTUTIL.format_table(rows)
        self.assertIn("A:", laid)
        self.assertIn("a1", laid)
        self.assertNotIn("|", laid)

    def test_format_table_two_by_three(self):
        rows = [
            ["", "Sg", "Pl"],
            ["1", "λύω", "λύομεν"],
            ["2", "λύεις", "λύετε"],
        ]
        laid = TEXTUTIL.format_table(rows, width_budget=42)
        lines = laid.splitlines()
        # Conjugation: vector block with person data rows only.
        self.assertTrue(lines[0].startswith(f"{TEXTUTIL.TABLE_RS}table "))
        self.assertIn("f=C", lines[0])
        self.assertEqual(lines[-1], TEXTUTIL.TABLE_END_LINE)
        cell_rows = [ln for ln in lines if TEXTUTIL.CELL_US in ln]
        self.assertEqual(len(cell_rows), 2)
        self.assertNotIn("Sg", laid)
        self.assertNotIn("Pl", laid)
        self.assertNotIn("+", laid)
        self.assertIn("λύω", laid)
        self.assertIn("λύομεν", laid)
        self.assertIn("1", cell_rows[0])
        self.assertIn("2", cell_rows[1])

    def test_plain_text_alias_uses_xfd(self):
        text = TEXTUTIL.plain_text("- Regel **A**")
        self.assertEqual(text, f"• Regel {TEXTUTIL.BOLD_ON}A{TEXTUTIL.BOLD_OFF}")

    def test_field_map_converts_markdown(self):
        fields = TEXTUTIL.field_map_from_pairs(
            [("Front", "## Titel\n\n| a | b |\n| - | - |\n| x | y |")]
        )
        self.assertIn("Front", fields)
        self.assertIn("Titel", fields["Front"])
        self.assertIn("x", fields["Front"])
        # Generic (non-conjugation) 2-col → vector block with header flag.
        self.assertIn(f"{TEXTUTIL.TABLE_RS}table ", fields["Front"])
        self.assertIn("f=H", fields["Front"])
        self.assertIn(TEXTUTIL.CELL_US, fields["Front"])
        self.assertNotIn("| - |", fields["Front"])
        self.assertNotIn("+", fields["Front"])

    def test_limit_still_applies(self):
        text = TEXTUTIL.to_device_text("x" * 100, limit=20)
        self.assertTrue(text.endswith("…"))
        self.assertLessEqual(len(text), 20)

    def test_link_text_only(self):
        text = TEXTUTIL.to_device_text('<a href="https://example.com">Lemma</a>')
        self.assertEqual(text, "Lemma")
        self.assertNotIn("http", text)


if __name__ == "__main__":
    unittest.main()
