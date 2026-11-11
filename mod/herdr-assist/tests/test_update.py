# cd mod/herdr-assist && python -m unittest tests.test_update -v
import hashlib
import io
import json
import os
import tarfile
import tempfile
import unittest
from unittest import mock

import update as u


def with_version(content):
    """Aponta o arquivo VERSION para um arquivo temporário com `content`."""
    d = tempfile.TemporaryDirectory()
    path = os.path.join(d.name, "VERSION")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(content)
    return d, mock.patch.object(u, "VERSION_FILE", path)


class TestNormalize(unittest.TestCase):
    def test_tira_o_v_da_tag(self):
        # o release carrega "v0.10.0", o arquivo VERSION traz "0.10.0";
        # sem normalizar, toda comparação acusaria diferença
        self.assertEqual(u.normalize("v0.10.0"), "0.10.0")
        self.assertEqual(u.normalize("0.10.0"), "0.10.0")
        self.assertEqual(u.normalize("  V1.2.3  "), "1.2.3")

    def test_vazio_e_none(self):
        self.assertEqual(u.normalize(""), "")
        self.assertEqual(u.normalize(None), "")


class TestInstalledVersion(unittest.TestCase):
    def test_le_o_arquivo_version(self):
        d, p = with_version("0.9.0\n")
        with d, p:
            self.assertEqual(u.installed_version(), "0.9.0")

    def test_sem_arquivo_devolve_vazio(self):
        with mock.patch.object(u, "VERSION_FILE", "/naoexiste/VERSION"):
            self.assertEqual(u.installed_version(), "")

    def test_arquivo_vazio_devolve_vazio(self):
        d, p = with_version("")
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
        self.assertEqual(u.should_update("0.9.0", "0.10.0"), (True, "available"))

    def test_mesma_versao_nao_faz_nada(self):
        self.assertEqual(u.should_update("0.9.0", "0.9.0"), (False, "same"))

    def test_versao_anterior_tambem_conta(self):
        # diferença, não ordem: um release de correção que aponta para trás
        # precisa chegar aos hosts (mesma regra do fw_update.c)
        self.assertEqual(u.should_update("0.10.0", "0.9.0"), (True, "available"))

    def test_versao_desconhecida_nao_arrisca(self):
        self.assertEqual(u.should_update("", "0.10.0"), (False, "unknown"))
        self.assertEqual(u.should_update("0.9.0", ""), (False, "unknown"))


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


class TestState(unittest.TestCase):
    def test_ida_e_volta(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "update.json")
            with mock.patch.object(u, "STATE", d), \
                 mock.patch.object(u, "STATE_FILE", path):
                u.save_state({"latest": "0.10.0"})
                self.assertEqual(u.load_state()["latest"], "0.10.0")

    def test_arquivo_corrompido_nao_derruba(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "update.json")
            with open(path, "w") as fh:
                fh.write("{quebrado")
            with mock.patch.object(u, "STATE_FILE", path):
                self.assertEqual(u.load_state(), {})


def make_archive(version, members=None):
    """Bytes do pacote do mod, como o release.yml o monta."""
    files = {"herdr-assist/VERSION": version + "\n", "herdr-assist/main.js": "// " + version}
    files.update(members or {})
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tf:
        for name, text in files.items():
            data = text.encode()
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tf.addfile(info, io.BytesIO(data))
    return buf.getvalue()


def sums_for(archive):
    """SHA256SUMS como o `sha256sum *.bin *.tar.gz` do release.yml grava."""
    return ("%s  herdr-assist-v0.10.0-install.bin\n%s  herdr-assist-mod.tar.gz\n"
            % ("0" * 64, hashlib.sha256(archive).hexdigest()))


def fake_fetch(version, sums=None, **kw):
    """Download falso: o pacote, ou o SHA256SUMS dele (`sums` troca o conteúdo)."""
    archive = make_archive(version, **kw)

    def fetch(url, dest):
        if url.endswith("/SHA256SUMS"):
            data = (sums_for(archive) if sums is None else sums).encode()
        else:
            data = archive
        with open(dest, "wb") as fh:
            fh.write(data)
    return fetch


class ModDir(unittest.TestCase):
    """Uma pasta de mod instalada (0.9.0) dentro de um pai temporário."""

    def setUp(self):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        self.parent = d.name
        self.mod = os.path.join(d.name, "herdr-assist")
        os.mkdir(self.mod)
        self.write("VERSION", "0.9.0\n")
        self.write("main.js", "// 0.9.0")

    def write(self, name, text):
        with open(os.path.join(self.mod, name), "w") as fh:
            fh.write(text)

    def read(self, name):
        with open(os.path.join(self.mod, name)) as fh:
            return fh.read()


class TestInstall(ModDir):
    def test_troca_a_pasta_e_nao_deixa_resto(self):
        self.assertTrue(u.install("0.10.0", self.mod, fake_fetch("0.10.0")))
        self.assertEqual(self.read("VERSION").strip(), "0.10.0")
        # nem backup nem a pasta de trabalho ficam ao lado
        self.assertEqual(os.listdir(self.parent), ["herdr-assist"])

    def test_falha_no_meio_da_troca_mantem_a_antiga(self):
        real, calls = os.rename, []

        def rename(a, b):
            calls.append(a)
            if len(calls) == 2:     # a pasta nova não entra no lugar
                raise OSError("disco cheio")
            real(a, b)

        with mock.patch.object(u.os, "rename", side_effect=rename):
            self.assertFalse(u.install("0.10.0", self.mod, fake_fetch("0.10.0")))
        self.assertEqual(self.read("main.js"), "// 0.9.0")
        self.assertEqual(os.listdir(self.parent), ["herdr-assist"])

    def test_backup_e_pasta_nova_sao_ocultos(self):
        # o app não recarrega pastas ocultas: um backup visível viraria um 2º mod
        real, names = os.rename, []

        def rename(a, b):
            names.extend([a, b])
            real(a, b)

        with mock.patch.object(u.os, "rename", side_effect=rename):
            self.assertTrue(u.install("0.10.0", self.mod, fake_fetch("0.10.0")))
        visible = {n for n in names if not os.path.relpath(n, self.parent).startswith(".")}
        self.assertEqual(visible, {self.mod})

    def test_download_falho_mantem_a_antiga(self):
        def boom(url, dest):
            raise OSError("sem rota")
        self.assertFalse(u.install("0.10.0", self.mod, boom))
        self.assertEqual(self.read("VERSION").strip(), "0.9.0")

    def test_pacote_com_versao_diferente_da_publicada_e_recusado(self):
        self.assertFalse(u.install("0.10.0", self.mod, fake_fetch("0.8.0")))
        self.assertEqual(self.read("VERSION").strip(), "0.9.0")

    def test_baixa_o_sha256sums_da_mesma_tag(self):
        urls = []
        inner = fake_fetch("0.10.0")
        self.assertTrue(u.install("0.10.0", self.mod,
                                  lambda url, dest: (urls.append(url), inner(url, dest))))
        self.assertEqual(sorted(urls), sorted([u.ASSET_URL.format("0.10.0"),
                                               u.SUMS_URL.format("0.10.0")]))

    def test_hash_divergente_nao_instala(self):
        bad = "%s  herdr-assist-mod.tar.gz\n" % ("f" * 64)
        self.assertFalse(u.install("0.10.0", self.mod, fake_fetch("0.10.0", sums=bad)))
        self.assertEqual(self.read("VERSION").strip(), "0.9.0")
        self.assertEqual(os.listdir(self.parent), ["herdr-assist"])

    def test_sem_hash_do_pacote_nao_instala(self):
        for sums in ("", "%s  outro.tar.gz\n" % ("0" * 64), "lixo\n"):
            self.assertFalse(u.install("0.10.0", self.mod, fake_fetch("0.10.0", sums=sums)),
                             sums)
        self.assertEqual(self.read("VERSION").strip(), "0.9.0")

    def test_sha256sums_em_modo_binario_tambem_vale(self):
        archive = make_archive("0.10.0")
        sums = "%s *herdr-assist-mod.tar.gz\n" % hashlib.sha256(archive).hexdigest()
        self.assertEqual(u.expected_sha256(sums), hashlib.sha256(archive).hexdigest())

    def test_pacote_que_escapa_da_pasta_e_recusado(self):
        evil = fake_fetch("0.10.0", members={"herdr-assist/../../fora": "x"})
        self.assertFalse(u.install("0.10.0", self.mod, evil))
        self.assertFalse(os.path.exists(os.path.join(self.parent, "..", "fora")))
        self.assertEqual(self.read("VERSION").strip(), "0.9.0")


class TestRecover(ModDir):
    def test_queda_entre_os_renames_restaura_o_backup(self):
        old = os.path.join(self.parent, ".herdr-assist.old")
        os.rename(self.mod, old)
        u.recover(self.mod)
        self.assertEqual(self.read("VERSION").strip(), "0.9.0")
        self.assertEqual(os.listdir(self.parent), ["herdr-assist"])

    def test_pasta_presente_nao_mexe_em_nada(self):
        old = os.path.join(self.parent, ".herdr-assist.old")
        os.mkdir(old)
        u.recover(self.mod)
        self.assertEqual(sorted(os.listdir(self.parent)), [".herdr-assist.old", "herdr-assist"])

    def test_sem_backup_nao_faz_nada(self):
        os.rename(self.mod, os.path.join(self.parent, "outra"))
        u.recover(self.mod)
        self.assertEqual(os.listdir(self.parent), ["outra"])


class TestIsDev(ModDir):
    def test_copia_instalada_com_tests_nao_e_dev(self):
        # a instalação por cópia do checkout leva tests/ e ainda assim atualiza
        os.mkdir(os.path.join(self.mod, "tests"))
        with mock.patch.object(u, "APP_MOD_DIR", self.mod):
            self.assertFalse(u.is_dev(self.mod))

    def test_symlink_no_caminho_do_app_e_dev(self):
        # o python resolve o symlink no __file__: o teste é no caminho do app
        link = os.path.join(self.parent, "app-link")
        os.symlink(self.mod, link)
        with mock.patch.object(u, "APP_MOD_DIR", link):
            self.assertTrue(u.is_dev(self.mod))

    def test_pasta_com_git_e_dev(self):
        os.mkdir(os.path.join(self.mod, ".git"))
        with mock.patch.object(u, "APP_MOD_DIR", self.mod):
            self.assertTrue(u.is_dev(self.mod))

    def test_rodando_fora_da_pasta_do_app_e_dev(self):
        # ponte avulsa do checkout: trocar essa pasta apagaria o trabalho
        with mock.patch.object(u, "APP_MOD_DIR", os.path.join(self.parent, "outra")):
            self.assertTrue(u.is_dev(self.mod))


class TestCheck(ModDir):
    """A rodada inteira, com a rede trocada por dublês."""

    def run_check(self, installed="0.9.0", published="0.10.0", app=None, auto="1"):
        fetch = mock.Mock(side_effect=fake_fetch(published))
        with tempfile.TemporaryDirectory() as sd, \
                mock.patch.object(u, "STATE", sd), \
                mock.patch.object(u, "STATE_FILE", os.path.join(sd, "update.json")), \
                mock.patch.object(u, "installed_version", return_value=installed), \
                mock.patch.object(u, "published_version", return_value=published), \
                mock.patch.dict(os.environ, {"AUTO_UPDATE": auto}), \
                mock.patch.object(u, "APP_MOD_DIR", app or self.mod):
            st = u.check(self.mod, fetch)
        return st, fetch

    def test_atualiza_quando_ha_versao_nova(self):
        st, fetch = self.run_check()
        self.assertEqual(st["state"], "installed")
        self.assertEqual(fetch.call_count, 2)   # pacote e SHA256SUMS
        self.assertEqual(self.read("VERSION").strip(), "0.10.0")

    def test_versao_igual_nao_atua(self):
        st, fetch = self.run_check(installed="0.10.0")
        self.assertEqual(st["state"], "same")
        fetch.assert_not_called()

    def test_auto_update_desligado_reporta_mas_nao_atua(self):
        st, fetch = self.run_check(auto="0")
        self.assertEqual(st["state"], "off")
        self.assertEqual(st["latest"], "0.10.0")   # a defasagem ainda aparece
        fetch.assert_not_called()
        self.assertEqual(self.read("VERSION").strip(), "0.9.0")

    def test_pasta_symlink_nao_atua(self):
        link = os.path.join(self.parent, "dev-link")
        os.symlink(self.mod, link)
        st, fetch = self.run_check(app=link)
        self.assertEqual(st["state"], "dev")
        fetch.assert_not_called()
        self.assertTrue(os.path.islink(link))

    def test_download_falho_registra_failed(self):
        with mock.patch.object(u, "install", return_value=False):
            st, _ = self.run_check()
        self.assertEqual(st["state"], "failed")

    def test_rede_fora_nao_apaga_a_ultima_versao_conhecida(self):
        with tempfile.TemporaryDirectory() as sd:
            path = os.path.join(sd, "update.json")
            with open(path, "w") as fh:
                json.dump({"latest": "0.10.0"}, fh)
            with mock.patch.object(u, "STATE", sd), mock.patch.object(u, "STATE_FILE", path), \
                    mock.patch.object(u, "installed_version", return_value="0.9.0"), \
                    mock.patch.object(u, "published_version", return_value=""):
                st = u.check(self.mod, mock.Mock())
        self.assertEqual((st["state"], st["latest"]), ("unknown", "0.10.0"))


if __name__ == "__main__":
    unittest.main()
