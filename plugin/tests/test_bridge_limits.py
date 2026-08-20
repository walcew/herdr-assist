# cd plugin && python -m unittest tests.test_bridge_limits -v
import os
import unittest
from unittest import mock

import herdr_bridge as b

FIX = os.path.join(os.path.dirname(__file__), "fixtures")


def fake_usage(url, headers):
    # o endpoint do Claude: dois limits normalizados
    return {"limits": [
        {"kind": "session", "percent": 12.4, "resets_at": "2026-08-20T10:00:00Z"},
        {"kind": "weekly_all", "percent": 40.0, "resets_at": "2026-08-27T10:00:00Z"},
    ]}


class TestCollectPorConta(unittest.TestCase):
    def test_claude_inclui_account(self):
        with mock.patch.object(b, "fetch_json", fake_usage):
            cur = b.collect_claude(os.path.join(FIX, "work"))
        self.assertEqual(cur["name"], "Claude")
        self.assertEqual(cur["account"], "bruno@work.gov.br")
        self.assertEqual(len(cur["limits"]), 2)

    def test_collect_limits_uma_por_conta_ordenado(self):
        dirs = [("claude", os.path.join(FIX, "personal")),
                ("claude", os.path.join(FIX, "work"))]
        with mock.patch.object(b, "fetch_json", fake_usage), \
             mock.patch.object(b, "HOME", FIX):
            providers = b.collect_limits(dirs)
        emails = [p["account"] for p in providers]
        self.assertEqual(emails, sorted(emails))               # determinístico
        self.assertIn("bruno@work.gov.br", emails)
        self.assertIn("bruno@pessoal.dev", emails)


if __name__ == "__main__":
    unittest.main()
