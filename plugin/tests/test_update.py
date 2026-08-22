# cd plugin && python -m unittest tests.test_update -v
import json
import os
import tempfile
import unittest
from unittest import mock

import update as u


def with_manifest(content):
    """Aponta o herdr-plugin.toml para um arquivo temporário com `content`."""
    d = tempfile.TemporaryDirectory()
    path = os.path.join(d.name, "herdr-plugin.toml")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(content)
    return d, mock.patch.object(u, "MANIFEST", path)


TOML = '''# comentário antes
id = "herdr-assist"
name = "herdr-assist bridge"
version = "0.9.0"
min_herdr_version = "0.8.0"

[[panes]]
id = "admin"
'''


class TestNormalize(unittest.TestCase):
    def test_tira_o_v_da_tag(self):
        # o release carrega "v0.10.0", o manifesto do plugin declara "0.10.0";
        # sem normalizar, toda comparação acusaria diferença
        self.assertEqual(u.normalize("v0.10.0"), "0.10.0")
        self.assertEqual(u.normalize("0.10.0"), "0.10.0")
        self.assertEqual(u.normalize("  V1.2.3  "), "1.2.3")

    def test_vazio_e_none(self):
        self.assertEqual(u.normalize(""), "")
        self.assertEqual(u.normalize(None), "")


class TestInstalledVersion(unittest.TestCase):
    def test_le_a_primeira_chave_version(self):
        d, p = with_manifest(TOML)
        with d, p:
            self.assertEqual(u.installed_version(), "0.9.0")

    def test_sem_arquivo_devolve_vazio(self):
        with mock.patch.object(u, "MANIFEST", "/naoexiste/herdr-plugin.toml"):
            self.assertEqual(u.installed_version(), "")

    def test_sem_chave_version_devolve_vazio(self):
        d, p = with_manifest('id = "herdr-assist"\n')
        with d, p:
            self.assertEqual(u.installed_version(), "")


class TestPublishedVersion(unittest.TestCase):
    def fake_urlopen(self, payload):
        body = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
        r = mock.MagicMock()
        r.read.return_value = body
        r.__enter__.return_value = r
        return mock.patch.object(u.urllib.request, "urlopen", return_value=r)

    def test_le_a_versao_do_manifesto(self):
        with self.fake_urlopen({"version": "v0.10.0", "url": "...", "size": 1}):
            self.assertEqual(u.published_version(), "0.10.0")

    def test_json_invalido_devolve_vazio(self):
        with self.fake_urlopen(b"<html>404</html>"):
            self.assertEqual(u.published_version(), "")

    def test_sem_campo_version_devolve_vazio(self):
        with self.fake_urlopen({"url": "...", "size": 1}):
            self.assertEqual(u.published_version(), "")

    def test_rede_fora_devolve_vazio(self):
        with mock.patch.object(u.urllib.request, "urlopen",
                               side_effect=OSError("sem rota")):
            self.assertEqual(u.published_version(), "")


class TestShouldUpdate(unittest.TestCase):
    def test_versao_nova_atualiza(self):
        self.assertEqual(u.should_update("0.9.0", "0.10.0", "github", ""),
                         (True, "available"))

    def test_mesma_versao_nao_faz_nada(self):
        self.assertEqual(u.should_update("0.9.0", "0.9.0", "github", ""),
                         (False, "same"))

    def test_versao_anterior_tambem_conta(self):
        # diferença, não ordem: um release de correção que aponta para trás
        # precisa chegar aos hosts (mesma regra do fw_update.c)
        go, why = u.should_update("0.10.0", "0.9.0", "github", "")
        self.assertTrue(go)
        self.assertEqual(why, "available")

    def test_checkout_local_nunca_e_tocado(self):
        # reinstalar por cima de um plugin linkado destruiria o ambiente de
        # trabalho de quem desenvolve
        self.assertEqual(u.should_update("0.9.0", "0.10.0", "local", ""),
                         (False, "local"))

    def test_nao_repete_versao_que_ja_falhou(self):
        # a rodada anterior tentou a 0.10.0 e a instalada continua 0.9.0: o
        # install não pegou, e repetir seria ciclar install→restart→install
        self.assertEqual(u.should_update("0.9.0", "0.10.0", "github", "0.10.0"),
                         (False, "tried"))

    def test_tentativa_antiga_nao_bloqueia_versao_nova(self):
        go, _ = u.should_update("0.9.0", "0.11.0", "github", "0.10.0")
        self.assertTrue(go)

    def test_versao_desconhecida_nao_arrisca(self):
        self.assertEqual(u.should_update("", "0.10.0", "github", ""),
                         (False, "unknown"))
        self.assertEqual(u.should_update("0.9.0", "", "github", ""),
                         (False, "unknown"))


class TestAutoEnabled(unittest.TestCase):
    def test_default_e_ligado(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertTrue(u.auto_enabled())

    def test_desliga_com_zero(self):
        for val in ("0", "false", "NO"):
            with mock.patch.dict(os.environ, {"AUTO_UPDATE": val}):
                self.assertFalse(u.auto_enabled(), val)

    def test_qualquer_outra_coisa_liga(self):
        with mock.patch.dict(os.environ, {"AUTO_UPDATE": "1"}):
            self.assertTrue(u.auto_enabled())


class TestPluginSource(unittest.TestCase):
    def fake_cli(self, source):
        payload = json.dumps({"result": {"plugins": [{"source": source}]}})
        return mock.patch.object(u, "_run", return_value=payload)

    def test_local(self):
        with self.fake_cli({"kind": "local"}):
            self.assertEqual(u.plugin_source("herdr")["kind"], "local")

    def test_github_com_repo(self):
        with self.fake_cli({"kind": "github", "repo": "walcew/herdr-assist/plugin"}):
            src = u.plugin_source("herdr")
            self.assertEqual(src["kind"], "github")
            self.assertEqual(src["repo"], "walcew/herdr-assist/plugin")

    def test_github_sem_repo_reconhecivel(self):
        # o formato do source para plugin do GitHub não pôde ser conferido na
        # máquina de desenvolvimento; sem palpite bom, quem chama usa o default
        with self.fake_cli({"kind": "github", "algo": "outra coisa"}):
            self.assertEqual(u.plugin_source("herdr")["repo"], "")

    def test_cli_fora_do_ar(self):
        with mock.patch.object(u, "_run", return_value=None):
            self.assertEqual(u.plugin_source("herdr"), {})


class TestState(unittest.TestCase):
    def test_ida_e_volta(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "update.json")
            with mock.patch.object(u, "STATE", d), \
                 mock.patch.object(u, "STATE_FILE", path):
                u.save_state({"latest": "0.10.0", "tried": "0.10.0"})
                self.assertEqual(u.load_state()["tried"], "0.10.0")

    def test_arquivo_corrompido_nao_derruba(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "update.json")
            with open(path, "w") as fh:
                fh.write("{quebrado")
            with mock.patch.object(u, "STATE_FILE", path):
                self.assertEqual(u.load_state(), {})


class TestCheck(unittest.TestCase):
    """A rodada inteira, com rede e CLI trocados por dublês."""

    def run_check(self, installed, published, kind, state=None, **kw):
        d = tempfile.TemporaryDirectory()
        path = os.path.join(d.name, "update.json")
        if state is not None:
            with open(path, "w") as fh:
                json.dump(state, fh)
        with d, mock.patch.object(u, "STATE", d.name), \
                mock.patch.object(u, "STATE_FILE", path), \
                mock.patch.object(u, "installed_version", return_value=installed), \
                mock.patch.object(u, "published_version", return_value=published), \
                mock.patch.object(u, "plugin_source", return_value={"kind": kind}), \
                mock.patch.object(u, "install", return_value=kw.get("ok", True)) as ins:
            st = u.check("herdr", force=kw.get("force", False))
            return st, ins

    def test_instala_quando_ha_versao_nova(self):
        with mock.patch.dict(os.environ, {"AUTO_UPDATE": "1"}):
            st, ins = self.run_check("0.9.0", "0.10.0", "github")
        self.assertEqual(st["state"], "installed")
        self.assertEqual(st["tried"], "0.10.0")
        ins.assert_called_once()

    def test_opt_out_reporta_mas_nao_age(self):
        with mock.patch.dict(os.environ, {"AUTO_UPDATE": "0"}):
            st, ins = self.run_check("0.9.0", "0.10.0", "github")
        self.assertEqual(st["state"], "off")
        self.assertEqual(st["latest"], "0.10.0")   # a defasagem ainda aparece
        ins.assert_not_called()

    def test_force_passa_por_cima_do_opt_out(self):
        with mock.patch.dict(os.environ, {"AUTO_UPDATE": "0"}):
            st, ins = self.run_check("0.9.0", "0.10.0", "github", force=True)
        self.assertEqual(st["state"], "installed")
        ins.assert_called_once()

    def test_force_nao_passa_por_cima_do_checkout_local(self):
        with mock.patch.dict(os.environ, {"AUTO_UPDATE": "1"}):
            st, ins = self.run_check("0.9.0", "0.10.0", "local", force=True)
        self.assertEqual(st["state"], "local")
        ins.assert_not_called()

    def test_install_falho_marca_a_tentativa(self):
        # o registro da tentativa é o que impede a rodada seguinte de repetir
        with mock.patch.dict(os.environ, {"AUTO_UPDATE": "1"}):
            st, _ = self.run_check("0.9.0", "0.10.0", "github", ok=False)
        self.assertEqual(st["state"], "failed")
        self.assertEqual(st["tried"], "0.10.0")

    def test_segunda_rodada_apos_falha_nao_repete(self):
        with mock.patch.dict(os.environ, {"AUTO_UPDATE": "1"}):
            st, ins = self.run_check("0.9.0", "0.10.0", "github",
                                     state={"tried": "0.10.0"})
        self.assertEqual(st["state"], "tried")
        ins.assert_not_called()

    def test_rede_fora_nao_apaga_a_ultima_versao_conhecida(self):
        with mock.patch.dict(os.environ, {"AUTO_UPDATE": "1"}):
            st, ins = self.run_check("0.9.0", "", "github",
                                     state={"latest": "0.10.0"})
        self.assertEqual(st["state"], "unknown")
        self.assertEqual(st["latest"], "0.10.0")
        ins.assert_not_called()


if __name__ == "__main__":
    unittest.main()
