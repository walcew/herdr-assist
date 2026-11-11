# cd plugin && python -m unittest tests.test_bridge_limits -v
import os
import unittest
from unittest import mock

import herdr_bridge as b

FIX = os.path.join(os.path.dirname(__file__), "fixtures")


# O Claude vem do CLI (claude_cli): auth status dá a identidade, /usage as
# janelas. Os dois são falsificados por config-dir, para os testes de conta.
def fake_auth_status(config_dir):
    email = "bruno@work.gov.br" if config_dir.endswith("work") else "bruno@pessoal.dev"
    return {"loggedIn": True, "email": email, "subscriptionType": "max"}


def fake_usage_rows(config_dir):
    return [{"label": "5h", "pct": 12, "resets_at": 1755684000, "window_s": 18000},
            {"label": "7d", "pct": 40, "resets_at": 1756288800, "window_s": 604800}]


def patch_cli():
    return (mock.patch.object(b.claude_cli, "auth_status", fake_auth_status),
            mock.patch.object(b.claude_cli, "usage_rows", fake_usage_rows))


class TestCollectPorConta(unittest.TestCase):
    def test_claude_inclui_account(self):
        auth, usage = patch_cli()
        with auth, usage:
            cur = b.collect_claude(os.path.join(FIX, "work"))
        self.assertEqual(cur["name"], "Claude")
        self.assertEqual(cur["account"], "bruno@work.gov.br")
        self.assertEqual(len(cur["limits"]), 2)

    def test_conta_sem_login_some_do_painel(self):
        with mock.patch.object(b.claude_cli, "auth_status",
                               lambda d: {"loggedIn": False}):
            providers = b.collect_limits([("claude", os.path.join(FIX, "work"))])
        self.assertEqual(providers, [])

    def test_collect_limits_uma_por_conta_ordenado(self):
        dirs = [("claude", os.path.join(FIX, "personal")),
                ("claude", os.path.join(FIX, "work"))]
        auth, usage = patch_cli()
        with auth, usage, mock.patch.object(b, "HOME", FIX):
            providers = b.collect_limits(dirs)
        emails = [p["account"] for p in providers]
        self.assertEqual(emails, sorted(emails))               # determinístico
        self.assertIn("bruno@work.gov.br", emails)
        self.assertIn("bruno@pessoal.dev", emails)


def fake_codex_usage(url, headers):
    return {"plan_type": "plus", "rate_limit": {
        "primary_window": {"limit_window_seconds": 18000, "used_percent": 20, "reset_at": 9999999999},
        "secondary_window": {"limit_window_seconds": 604800, "used_percent": 40, "reset_at": 9999999999}}}


class TestWindowS(unittest.TestCase):
    def test_claude_window_s_por_janela(self):
        auth, usage = patch_cli()
        with auth, usage:
            cur = b.collect_claude(os.path.join(FIX, "work"))
        ws = {r["label"]: r["window_s"] for r in cur["limits"]}
        self.assertEqual(ws["5h"], 18000)     # sessão
        self.assertEqual(ws["7d"], 604800)    # semana

    def test_codex_window_s_do_limit_window_seconds(self):
        with mock.patch.object(b, "fetch_json", fake_codex_usage):
            cur = b.collect_codex(os.path.join(FIX, "codex"))
        ws = sorted(r["window_s"] for r in cur["limits"])
        self.assertEqual(ws, [18000, 604800])


class TestLimitsOrgCorp(unittest.TestCase):
    def test_provider_tem_org_corp(self):
        # collect_claude não faz org/corp; o enriquecimento é no payload.
        # Testar a função de enriquecimento pura:
        prov = {"name": "Claude", "account": "bruno@sosdocs.com.br"}
        b.enrich_provider(prov, agents=3, working=2)
        self.assertEqual(prov["org"], "sosdocs")
        self.assertTrue(prov["corp"])
        self.assertEqual(prov["agents"], 3)
        self.assertEqual(prov["agents_working"], 2)


if __name__ == "__main__":
    unittest.main()
