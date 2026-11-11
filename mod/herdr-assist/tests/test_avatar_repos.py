# cd mod/herdr-assist && python3 -m unittest tests.test_avatar_repos -v
import os
import tempfile
import unittest

import herdr_bridge as b


def read(content):
    """Lê o arquivo `avatar_repos` legado (migração) com `content`."""
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "avatar_repos")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(content)
        return b.read_avatar_repos_file(path)


class TestAvatarRepos(unittest.TestCase):
    def test_sem_arquivo_devolve_vazio(self):
        # caminho normal: só quem publica avatares cria o arquivo
        self.assertEqual(b.read_avatar_repos_file("/naoexiste/avatar_repos"), [])

    def test_ignora_comentario_e_linha_vazia(self):
        self.assertEqual(read("# meus repos\n\n  https://a.example/x/  \n\n#https://off\n"),
                         ["https://a.example/x/"])

    def test_respeita_o_teto_do_firmware(self):
        # o painel guarda STORE_BRIDGE_REPOS por ponte; mandar mais só engordaria
        # o payload para ser cortado do outro lado
        urls = read("\n".join("https://r%d.example/" % i for i in range(5)))
        self.assertEqual(len(urls), b.AVATAR_REPOS_MAX)
        self.assertEqual(urls[0], "https://r0.example/")


if __name__ == "__main__":
    unittest.main()
