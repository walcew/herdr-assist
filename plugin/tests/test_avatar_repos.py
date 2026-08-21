# cd plugin && python -m unittest tests.test_avatar_repos -v
import os
import tempfile
import unittest
from unittest import mock

import herdr_bridge as b


def with_file(content):
    """Aponta AVATAR_REPOS_FILE para um arquivo temporário com `content`."""
    d = tempfile.TemporaryDirectory()
    path = os.path.join(d.name, "avatar_repos")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(content)
    return d, mock.patch.object(b, "AVATAR_REPOS_FILE", path)


class TestAvatarRepos(unittest.TestCase):
    def test_sem_arquivo_devolve_vazio(self):
        # caminho normal: só quem publica avatares cria o arquivo
        with mock.patch.object(b, "AVATAR_REPOS_FILE", "/naoexiste/avatar_repos"):
            self.assertEqual(b.avatar_repos(), [])

    def test_ignora_comentario_e_linha_vazia(self):
        d, p = with_file("# meus repos\n\n  https://a.example/x/  \n\n#https://off\n")
        with d, p:
            self.assertEqual(b.avatar_repos(), ["https://a.example/x/"])

    def test_respeita_o_teto_do_firmware(self):
        # o painel guarda STORE_BRIDGE_REPOS por ponte; mandar mais só engordaria
        # o payload para ser cortado do outro lado
        d, p = with_file("\n".join("https://r%d.example/" % i for i in range(5)))
        with d, p:
            urls = b.avatar_repos()
            self.assertEqual(len(urls), b.AVATAR_REPOS_MAX)
            self.assertEqual(urls[0], "https://r0.example/")


if __name__ == "__main__":
    unittest.main()
