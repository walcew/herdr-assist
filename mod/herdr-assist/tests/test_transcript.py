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

    def test_janela_por_modelo(self):
        # família 5 é 1M documentado; antes 150k virava 75% (divisão por 200k)
        self.assertEqual(transcript.context_pct(150000, "claude-opus-5"), 15)
        self.assertEqual(transcript.context_pct(150000, "claude-fable-5"), 15)
        self.assertEqual(transcript.context_pct(500000, "claude-opus-4-8"), 50)
        self.assertEqual(transcript.context_pct(150000, "claude-haiku-4-5"), 75)

    def test_modelo_fora_da_tabela_mantem_o_degrau(self):
        self.assertEqual(transcript.context_pct(150000, "claude-sonnet-4-5"), 75)
        self.assertEqual(transcript.context_pct(489422, "claude-sonnet-4-5"), 48)


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

    def test_compact_depois_do_ultimo_assistant_usa_posttokens(self):
        # /compact sem prompt seguinte: a barra cai na hora, não no próximo
        # turno — antes segurava o valor pré-compact indefinidamente
        with tempfile.TemporaryDirectory() as d:
            fp = os.path.join(d, "t.jsonl")
            with open(fp, "w", encoding="utf-8") as fh:
                fh.write('{"type":"assistant","message":{"role":"assistant","model":"claude-opus-5","usage":{"input_tokens":900000}}}\n')
                fh.write('{"type":"system","subtype":"compact_boundary","compactMetadata":{"trigger":"manual","preTokens":900000,"postTokens":12348}}\n')
            m = transcript.session_metrics(fp)
            self.assertEqual(m["context_pct"], 1)      # 12348 de 1M
            self.assertEqual(m["model"], "Opus 5")     # modelo segue do assistant

    def test_compact_antes_do_assistant_nao_interfere(self):
        with tempfile.TemporaryDirectory() as d:
            fp = os.path.join(d, "t.jsonl")
            with open(fp, "w", encoding="utf-8") as fh:
                fh.write('{"type":"system","subtype":"compact_boundary","compactMetadata":{"postTokens":12348}}\n')
                fh.write('{"type":"assistant","message":{"role":"assistant","model":"claude-opus-5","usage":{"input_tokens":150000}}}\n')
            self.assertEqual(transcript.session_metrics(fp)["context_pct"], 15)

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
