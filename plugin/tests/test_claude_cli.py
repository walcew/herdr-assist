"""Parsing do /usage do CLI do Claude Code (funções puras de claude_cli).

Rodar de dentro de plugin/: python -m unittest discover -s tests
"""
import datetime
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import claude_cli  # noqa: E402

FIX = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures", "usage")
REF = datetime.datetime(2026, 8, 20, 22, 0, 0)


def fixture(name):
    with open(os.path.join(FIX, name), encoding="utf-8") as fh:
        return fh.read()


class TestParseUsage(unittest.TestCase):
    def test_tres_janelas_da_conta_logada(self):
        rows = claude_cli.parse_usage(fixture("logged_in.txt"), REF)
        self.assertEqual([r["label"] for r in rows], ["5h", "7d", "7d Fable"])
        self.assertEqual([r["pct"] for r in rows], [52, 2, 0])

    def test_janela_em_segundos_por_tipo(self):
        rows = claude_cli.parse_usage(fixture("logged_in.txt"), REF)
        self.assertEqual([r["window_s"] for r in rows], [18000, 604800, 604800])

    def test_linha_sem_reset_vira_zero(self):
        rows = claude_cli.parse_usage(fixture("logged_in.txt"), REF)
        self.assertEqual(rows[2]["resets_at"], 0)   # "Current week (Fable)"
        self.assertGreater(rows[0]["resets_at"], 0)

    def test_sem_assinatura_nao_devolve_linha(self):
        self.assertEqual(claude_cli.parse_usage(fixture("no_subscription.txt"), REF), [])

    def test_texto_vazio_ou_lixo(self):
        self.assertEqual(claude_cli.parse_usage("", REF), [])
        self.assertEqual(claude_cli.parse_usage("erro qualquer\n", REF), [])

    def test_label_de_modelo_cabe_no_buffer_do_firmware(self):
        rows = claude_cli.parse_usage(
            "Current week (Modelo De Nome Muito Longo): 7% used\n", REF)
        self.assertLessEqual(len(rows[0]["label"]), 16)


class TestParseReset(unittest.TestCase):
    def test_hora_local_am(self):
        got = claude_cli.parse_reset("Aug 21 at 12:29am (America/Sao_Paulo)", REF)
        self.assertEqual(datetime.datetime.fromtimestamp(got),
                         datetime.datetime(2026, 8, 21, 0, 29))

    def test_hora_local_pm(self):
        got = claude_cli.parse_reset("Aug 27 at 9:59pm (America/Sao_Paulo)", REF)
        self.assertEqual(datetime.datetime.fromtimestamp(got),
                         datetime.datetime(2026, 8, 27, 21, 59))

    def test_meio_dia_e_meia_noite(self):
        meia_noite = claude_cli.parse_reset("Aug 21 at 12:00am", REF)
        meio_dia = claude_cli.parse_reset("Aug 21 at 12:00pm", REF)
        self.assertEqual(datetime.datetime.fromtimestamp(meia_noite).hour, 0)
        self.assertEqual(datetime.datetime.fromtimestamp(meio_dia).hour, 12)

    def test_vira_o_ano_quando_a_data_ja_passou(self):
        ref = datetime.datetime(2026, 12, 30, 23, 0)
        got = claude_cli.parse_reset("Jan 2 at 3:00am", ref)
        self.assertEqual(datetime.datetime.fromtimestamp(got).year, 2027)

    def test_sem_fuso_no_texto(self):
        self.assertGreater(claude_cli.parse_reset("Aug 21 at 1:05pm", REF), 0)

    def test_texto_irreconhecivel_vira_zero(self):
        self.assertEqual(claude_cli.parse_reset("", REF), 0)
        self.assertEqual(claude_cli.parse_reset("amanhã de manhã", REF), 0)
        self.assertEqual(claude_cli.parse_reset("Xyz 99 at 25:00am", REF), 0)


class TestConfigDirEnv(unittest.TestCase):
    """A conta default NÃO pode receber CLAUDE_CONFIG_DIR: com a variável, o
    CLI ignora o cofre do sistema (Keychain no macOS) e se diz deslogado."""

    HOME = os.path.join(os.sep, "home", "u")

    def test_default_e_reconhecida(self):
        self.assertTrue(claude_cli.is_default_dir(
            os.path.join(self.HOME, ".claude"), self.HOME))

    def test_conta_alternativa_nao_e_default(self):
        self.assertFalse(claude_cli.is_default_dir(
            os.path.join(self.HOME, ".claude-work"), self.HOME))

    def test_caminho_com_ponto_ou_barra_final(self):
        sujo = os.path.join(self.HOME, ".", ".claude") + os.sep
        self.assertTrue(claude_cli.is_default_dir(sujo, self.HOME))


if __name__ == "__main__":
    unittest.main()
