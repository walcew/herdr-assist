# cd mod/herdr-assist && python3 -m unittest tests.test_repaint -v
"""repaint_to_ansi: a repintura do attach observe vira o SGR que o painel parseia."""
import os
import unittest

import herdr_bridge as b

FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures",
                       "repaint-claude.bin")


class TestRepaintToAnsi(unittest.TestCase):
    def test_fixture_do_claude_code_mantem_so_texto_e_sgr(self):
        with open(FIXTURE, "rb") as fh:
            out = b.repaint_to_ansi(fh.read())
        lines = out.split("\n")
        self.assertEqual(len(lines), 18)              # a tela 42x18 inteira
        self.assertEqual(lines[0], "")                # 1ª linha da tela é vazia
        self.assertTrue(lines[1].startswith("\x1b[0m\x1b[38;2;215;119;87m ▐"))
        self.assertIn("\x1b[1mClaude Code ", lines[1])
        self.assertIn("bypass permissions on", lines[-1])
        for junk in ("\x1b[>4;2m", "\x1b[15;3H", "\x1b]", "\x1b[?", "\x1b[=", "\x1b(",
                     "\x18", "\r", "\x1b[9G", "\x1bH"):
            self.assertNotIn(junk, out)

    def test_conteudo_na_primeira_linha_e_cursor_viram_espacos(self):
        raw = b"\x1b[2J\x1b[3g\x1b[9G\x1bH\x1b[Hab\x1b[5Gc\x1b[2Cd\r\n\x1b[1;31mx\x1b[0m\x07"
        self.assertEqual(b.repaint_to_ansi(raw), "ab  c  d\n\x1b[1;31mx\x1b[0m")

    def test_trim_ansi_ignora_linhas_so_com_sgr_no_fim(self):
        self.assertEqual(b.trim_ansi("a\nb\nc\n\x1b[0m\n\n", 2), "b\nc")
