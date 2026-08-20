import os
import unittest
import accounts

FIX = os.path.join(os.path.dirname(__file__), "fixtures")


class TestResolveConfigDir(unittest.TestCase):
    def test_usa_env_quando_presente(self):
        env = {"CLAUDE_CONFIG_DIR": "/x/.claude-sos"}
        self.assertEqual(accounts.resolve_config_dir("claude", env, "/home/u"),
                         os.path.normpath(os.path.abspath("/x/.claude-sos")))

    def test_cai_no_default_sem_env(self):
        got = accounts.resolve_config_dir("codex", {}, "/home/u")
        self.assertEqual(got, os.path.normpath(os.path.abspath("/home/u/.codex")))


class TestReadEmail(unittest.TestCase):
    def test_le_email_do_config_dir(self):
        got = accounts.read_account_email("claude", os.path.join(FIX, "work"), FIX)
        self.assertEqual(got, "bruno@work.gov.br")

    def test_email_vazio_quando_ausente(self):
        got = accounts.read_account_email("claude", os.path.join(FIX, "naoexiste"), FIX)
        self.assertEqual(got, "")


class TestDiscover(unittest.TestCase):
    def test_descobre_contas_e_mapeia_panes(self):
        panes = [
            {"pane_id": "wY:p1", "agent": "claude"},   # pessoal (sem env)
            {"pane_id": "wZ:p1", "agent": "claude"},   # work (env)
            {"pane_id": "wX:p1", "agent": "codex"},    # codex default
            {"pane_id": "wA:p1", "agent": None},       # sem agente, ignorado
        ]
        envs = {75336: {"CLAUDE_CONFIG_DIR": "/h/.claude-sos"}}
        pids = {"wY:p1": 100, "wZ:p1": 75336, "wX:p1": 200}
        pane_account, account_dirs = accounts.discover(
            panes, lambda pid: pids.get(pid), lambda pid: envs.get(pid, {}), "/h")

        self.assertEqual(pane_account["wZ:p1"],
                         ("claude", os.path.normpath(os.path.abspath("/h/.claude-sos"))))
        self.assertEqual(pane_account["wY:p1"],
                         ("claude", os.path.normpath(os.path.abspath("/h/.claude"))))
        self.assertNotIn("wA:p1", pane_account)
        # defaults sempre presentes + a de work
        self.assertIn(("claude", os.path.normpath(os.path.abspath("/h/.claude"))), account_dirs)
        self.assertIn(("codex", os.path.normpath(os.path.abspath("/h/.codex"))), account_dirs)
        self.assertIn(("claude", os.path.normpath(os.path.abspath("/h/.claude-sos"))), account_dirs)


if __name__ == "__main__":
    unittest.main()
