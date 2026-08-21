# cd plugin && python -m unittest tests.test_transcript -v
import os, unittest, tempfile
import transcript

FIX = os.path.join(os.path.dirname(__file__), "fixtures", "transcript", "sample.jsonl")


class TestModelDisplay(unittest.TestCase):
    def test_mapeamentos(self):
        self.assertEqual(transcript.model_display("claude-opus-4-8"), "Opus 4.8")
        self.assertEqual(transcript.model_display("claude-sonnet-4-5"), "Sonnet 4.5")
        self.assertEqual(transcript.model_display("claude-haiku-4-5"), "Haiku 4.5")
        self.assertEqual(transcript.model_display("gpt-5"), "gpt-5")

    def test_desconhecido_devolve_id_clipado(self):
        self.assertEqual(transcript.model_display("modelo-xyz"), "modelo-xyz")


class TestContextPct(unittest.TestCase):
    def test_1M_quando_acima_de_200k(self):
        self.assertEqual(transcript.context_pct(489422), 48)   # 489422/1_000_000

    def test_200k_quando_abaixo(self):
        self.assertEqual(transcript.context_pct(150000), 75)   # 150000/200000

    def test_clamp_100(self):
        self.assertEqual(transcript.context_pct(2_000_000), 100)


class TestEncodeCwd(unittest.TestCase):
    def test_windows_path(self):
        self.assertEqual(
            transcript.encode_cwd(r"C:\Users\bruno\OneDrive\git\SOS\git-lab\spgg\docz\procergs"),
            "C--Users-bruno-OneDrive-git-SOS-git-lab-spgg-docz-procergs")

    def test_tira_barra_final(self):
        self.assertEqual(transcript.encode_cwd("C:\\a\\b\\"), "C--a-b")

    def test_preserva_hifen_existente(self):
        self.assertEqual(transcript.encode_cwd("/x/git-lab/y"), "-x-git-lab-y")


class TestSessionMetrics(unittest.TestCase):
    def test_le_ultimo_assistant(self):
        m = transcript.session_metrics(FIX)
        self.assertEqual(m["model"], "Opus 4.8")
        self.assertEqual(m["context_pct"], transcript.context_pct(2 + 488968 + 452))

    def test_effort_vem_do_nivel_superior(self):
        # regressão: effort é irmão de "message", não campo dentro dela — ler do
        # lugar errado devolveria "" calado, e a fixture sem o campo passaria
        with tempfile.TemporaryDirectory() as d:
            fp = os.path.join(d, "t.jsonl")
            with open(fp, "w", encoding="utf-8") as fh:
                fh.write('{"type":"assistant","effort":"max","message":{"role":"assistant","model":"claude-opus-5","usage":{"input_tokens":10}}}\n')
            self.assertEqual(transcript.session_metrics(fp)["effort"], "max")

    def test_sem_effort_devolve_vazio(self):
        # caminho normal do Codex e de CLI anterior à 2.1.234
        self.assertEqual(transcript.session_metrics(FIX)["effort"], "")

    def test_arquivo_ausente_devolve_none(self):
        self.assertIsNone(transcript.session_metrics("/naoexiste.jsonl"))

    def test_sem_assistant_devolve_none(self):
        p = os.path.join(os.path.dirname(__file__), "fixtures", "transcript", "no_assistant.jsonl")
        self.assertIsNone(transcript.session_metrics(p))

    def test_linha_json_nao_dict_nao_quebra(self):
        # regressão: linha [1,2,3] não pode derrubar a leitura
        with tempfile.TemporaryDirectory() as d:
            fp = os.path.join(d, "t.jsonl")
            with open(fp, "w", encoding="utf-8") as fh:
                fh.write("[1,2,3]\n")
                fh.write('{"type":"assistant","message":{"role":"assistant","model":"claude-opus-4-8","usage":{"input_tokens":10,"output_tokens":5}}}\n')
            m = transcript.session_metrics(fp)
            self.assertIsNotNone(m)
            self.assertEqual(m["model"], "Opus 4.8")


if __name__ == "__main__":
    unittest.main()
