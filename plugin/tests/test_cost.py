# cd plugin && python -m unittest tests.test_cost -v
import os, unittest
import cost

FIX = os.path.join(os.path.dirname(__file__), "fixtures", "cost", "sample.jsonl")


class TestMessageCost(unittest.TestCase):
    def test_opus(self):
        # 1000*5 + 2000*25 = 55000 → /1e6 = 0.055
        c = cost.message_cost({"input_tokens": 1000, "output_tokens": 2000}, "claude-opus-4-8")
        self.assertAlmostEqual(c, 0.055, places=6)

    def test_cache_read_e_write(self):
        # cache_read 0.1x, cache_creation 1.25x do input price
        c = cost.message_cost({"cache_read_input_tokens": 1_000_000,
                               "cache_creation_input_tokens": 1_000_000}, "claude-opus-4-8")
        self.assertAlmostEqual(c, 5*0.1 + 5*1.25, places=6)  # 0.5 + 6.25

    def test_modelo_desconhecido_usa_default(self):
        c = cost.message_cost({"input_tokens": 1_000_000}, "modelo-xyz")
        self.assertAlmostEqual(c, 5.0, places=6)  # default (5, 25)


class TestFmtUsd(unittest.TestCase):
    def test_decimais_virgula(self):
        self.assertEqual(cost.fmt_usd(4.2), "~US$ 4,20")

    def test_milhar_ponto(self):
        self.assertEqual(cost.fmt_usd(1240), "~US$ 1.240")


class TestFileCost(unittest.TestCase):
    def test_total_e_days(self):
        m = cost.file_cost(FIX)
        # opus: 1000*5+2000*25=55000/1e6=0.055 ; haiku: 1000*1=1000/1e6=0.001
        self.assertAlmostEqual(m["total"], 0.056, places=6)
        self.assertAlmostEqual(m["days"]["2026-08-20"], 0.055, places=6)
        self.assertAlmostEqual(m["days"]["2026-08-19"], 0.001, places=6)

    def test_ausente_none(self):
        self.assertIsNone(cost.file_cost("/naoexiste.jsonl"))


if __name__ == "__main__":
    unittest.main()
