# cd mod/herdr-assist && python3 -m unittest tests.test_friendly_name -v
"""Nome que o painel mostra para o agente: o rótulo da tab, senão o cwd."""
import unittest

import herdr_bridge as b


class TestFriendlyName(unittest.TestCase):
    def test_rotulo_da_aba_vence(self):
        self.assertEqual(b.friendly_name("agent-claude", "/tmp"), "agent-claude")

    def test_aba_sem_nome_cai_no_cwd(self):
        self.assertEqual(b.friendly_name("1", "/home/luis"), "luis")

    def test_sem_rotulo_cai_no_cwd(self):
        self.assertEqual(b.friendly_name("", "/home/luis"), "luis")


if __name__ == "__main__":
    unittest.main()
