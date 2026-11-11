import unittest
from unittest import mock
from proc_env import parse_proc_environ, parse_ps_env
import proc_env


class TestParseProcEnviron(unittest.TestCase):
    def test_pares_separados_por_nul(self):
        raw = b"PATH=/usr/bin\x00CLAUDE_CONFIG_DIR=/home/u/.claude-work\x00"
        env = parse_proc_environ(raw)
        self.assertEqual(env["CLAUDE_CONFIG_DIR"], "/home/u/.claude-work")
        self.assertEqual(env["PATH"], "/usr/bin")

    def test_ignora_entrada_sem_igual(self):
        raw = b"SOZINHO\x00A=1\x00"
        self.assertEqual(parse_proc_environ(raw), {"A": "1"})

    def test_valor_com_igual_preserva_o_resto(self):
        raw = b"Q=a=b=c\x00"
        self.assertEqual(parse_proc_environ(raw)["Q"], "a=b=c")


class TestParsePsEnv(unittest.TestCase):
    def test_extrai_config_dir(self):
        text = "PATH=/usr/bin CODEX_HOME=/Users/u/.codex-work TERM=xterm"
        env = parse_ps_env(text)
        self.assertEqual(env["CODEX_HOME"], "/Users/u/.codex-work")


class TestReadProcessEnvFalha(unittest.TestCase):
    def test_retorna_vazio_quando_reader_do_so_levanta(self):
        # o dispatcher nunca pode propagar exceção — contrato {} em qualquer falha
        with mock.patch.object(proc_env, "_read_windows", side_effect=OSError("boom")):
            self.assertEqual(proc_env.read_process_env(123), {})


if __name__ == "__main__":
    unittest.main()
