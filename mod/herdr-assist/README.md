# Mod herdr-assist (Herdr desktop)

Mod do app Herdr desktop (`herdr-app` + herdr-core) que cuida da **Ponte**: o processo
Python que traduz a API do core (`hc connect`) para o TCP autenticado que o painel e o
pager e-paper consomem. O mod mostra o estado num indicador no rodapé, abre um dialog de
controle (Status, Token, Parear) e mantém a Ponte viva como terminal do core. Python puro,
só stdlib (3.9+), sem toolchain.

> O plugin do **Herdr TUI** ficou congelado na tag `v0.14.1`. Ele não enxerga os agentes
> do app desktop; a seção [Saindo do Herdr TUI](#saindo-do-herdr-tui) cobre a troca.

## Instalação

1. Copie a pasta para o home do app (do checkout, ou do asset `herdr-assist-mod.tar.gz` do
   [release mais novo](https://github.com/walcew/herdr-assist/releases/latest), que já traz
   a pasta `herdr-assist/`):

   ```sh
   cp -R mod/herdr-assist ~/.config/herdr-app/mods/herdr-assist
   # para desenvolver, com hot reload (o app recarrega ao salvar):
   ln -s "$PWD/mod/herdr-assist" ~/.config/herdr-app/mods/herdr-assist
   ```

2. Mescle o `keys.json` do mod no da pessoa (`~/.config/herdr-app/keys.json`): acrescente
   `"cmd-alt-a": "mod.open:herdr-assist"` em `"global"`, sem trocar uma tecla que você já usa.
   Sem tecla, `herdr-app do mod.open --name herdr-assist` abre o mesmo dialog (chamar de novo
   fecha).

3. Na primeira vez que o app vê a pasta, o `mod.json` (`place: "bottom"`, 28 px) encaixa o
   **indicador** no rodapé do layout ativo: um chip com o estado da Ponte e o número de
   painéis; o clique abre o dialog. Essa cópia é quem sobe a Ponte quando o app abre (um
   mod `place: "none"` só roda com o dialog aberto, e o app não tem autostart de mod).

### Se o indicador conflitar com o layout

Se o encaixe automático (`place` + `overlay`) atrapalhar o seu layout, ponha o indicador à
mão: troque `"place": "bottom"` por `"none"` no `mod.json` e acrescente este bloco ao
layout, no ponto que quiser:

```json
{ "id": "herdr-assist", "type": "block", "size": { "px": 28 }, "content": { "mod": "herdr-assist" } }
```

O `overlay` do `mod.json` continua valendo: o dialog abre igual. A atualização automática
troca a pasta inteira, `mod.json` incluso; se o encaixe voltar depois de uma atualização,
repita a edição.

## Saindo do Herdr TUI

Uma vez só, em quem ainda tem o plugin do Herdr TUI:

```sh
herdr plugin disable herdr-assist     # no Herdr TUI: ele deixa de subir a ponte antiga
```

Depois pare a ponte antiga, que segue de pé com a porta 9375. O dialog de controle avisa
(`Porta 9375 ocupada por outra ponte (provavelmente a do Herdr TUI): pid …`) e oferece
**Encerrar**, com confirmação; nada é morto sozinho. Na mão:

```sh
lsof -nP -iTCP:9375 -sTCP:LISTEN      # quem escuta
kill <pid>
```

Na primeira subida (doc sem token) a Ponte nova importa `token`, `env` (porta, `AUTO_UPDATE`)
e `avatar_repos` do config-dir do plugin (`~/.config/herdr/plugins/config/herdr-assist/`)
ou do modo avulso (`~/.config/herdr-assist/`) para o doc do core, então os painéis já pareados conectam sem parear de novo.

## Configuração

Fica no doc `herdr-assist` do core, que a Ponte relê ao receber o evento `doc` (a troca de
porta reinicia o listener, o resto vale na hora):

| Campo | Padrão | O que faz |
|---|---|---|
| `token` | gerado | segredo do handshake dos painéis (aba Token do dialog mostra e rotaciona) |
| `port` | `9375` | porta TCP (painéis) e UDP (descoberta) |
| `avatar_repos` | `[]` | URLs de repositórios de avatar empurradas aos painéis |
| `auto_update` | `true` | atualização automática do mod (veja abaixo) |

Ler e gravar pela API do core (`doc.set` troca o valor inteiro: leia, altere e devolva com
a `version` lida):

```sh
echo '{"jsonrpc":"2.0","id":1,"method":"doc.get","params":{"name":"herdr-assist"}}' | hc connect
echo '{"jsonrpc":"2.0","id":1,"method":"doc.set","params":{"name":"herdr-assist","version":3,"value":{"token":"…","port":9375,"avatar_repos":[],"auto_update":false}}}' | hc connect
```

Variáveis de ambiente que a Ponte lê: `BRIDGE_BIND` (padrão `0.0.0.0`; é o que faz a
descoberta por broadcast funcionar), `HC_BIN` e `HERDR_APP_BIN` (binários, com fallback
no PATH); `BRIDGE_TOKEN` e `BRIDGE_PORT` valem sobre o doc, para dev e teste.

## Atualização automática

Uma vez por dia a Ponte compara o `VERSION` da pasta com a versão do release mais novo,
baixa `herdr-assist-mod.tar.gz` e o `SHA256SUMS` da mesma tag, confere o sha256 do pacote
(sem a linha dele ou divergente, não instala), extrai ao lado (no mesmo diretório), troca a
pasta por rename com um backup oculto (`.herdr-assist.old`) e reinicia a Ponte
(`ctl.py restart`). Se algo falha no meio, a pasta antiga fica; se a máquina cai entre os
dois renames, a próxima subida do `ctl.py` ou da Ponte restaura o backup.

Só a cópia em `~/.config/herdr-app/mods/herdr-assist` (ou `$HERDR_APP_HOME/mods/…`) se
atualiza. Não atua com `auto_update: false`, quando esse caminho é um symlink, quando a
pasta tem `.git` nem quando a Ponte roda de outra pasta: ali quem atualiza é o `git pull`.
A cópia comum (`cp -R` do checkout, com `tests/` e tudo) se atualiza normalmente.

## Ver o que está acontecendo

```sh
python3 ctl.py status                      # Ponte, versão (disco e no ar), porta, painéis, quem escuta (JSON)
python3 ctl.py discover                    # painéis em modo de pareamento na LAN (JSON)
hc ls --label herdr-assist=bridge          # o terminal da Ponte (endereço, estado)
hc read <endereço> --lines 200             # o log da Ponte: é a saída do terminal
```

Reiniciar sem o dialog: `python3 ctl.py restart`. Uma Ponte saudável loga, em ordem:
`bridge version …`, `udp discovery on …`, `bridge listening on 0.0.0.0:9375` e, a cada painel
que entra, `panel connected: ('<ip>', <porta>)`.

## Firewall

A máquina da Ponte precisa aceitar **TCP e UDP** na porta (9375 por padrão), na entrada:
TCP é a conexão dos painéis, UDP é a descoberta por broadcast. No macOS, o firewall pergunta
uma vez se o `python3` pode receber conexões: permita. No Windows com WSL o firewall comum
não governa isso — o Hyper-V firewall bloqueia entrada por padrão:

```powershell
New-NetFirewallHyperVRule -Name "herdr-assist"     -Direction Inbound -Protocol TCP -LocalPorts 9375 -Action Allow
New-NetFirewallHyperVRule -Name "herdr-assist-udp" -Direction Inbound -Protocol UDP -LocalPorts 9375 -Action Allow
```

Sem a regra UDP a Ponte sobe, escuta, e o painel fica em `OFFLINE` sem erro nenhum do lado
da Ponte. Sem a TCP ele encontra e não conecta.

## Problemas comuns

| Sintoma | Causa provável |
|---|---|
| Aviso de porta ocupada no dialog | a ponte do Herdr TUI ainda está de pé: veja [Saindo do Herdr TUI](#saindo-do-herdr-tui) |
| Painéis em `OFFLINE` ao abrir o app, sem indicador no layout | a Ponte só sobe quando há uma cópia do mod viva: abra o dialog com `cmd-alt-a` ou ponha o bloco do indicador no layout |
| Painel pareado em `OFFLINE`, log sem erro | falta a regra **UDP** (a descoberta não chega), ou o token foi rotacionado |
| Mod sumiu de `mods/` depois de uma atualização, `.herdr-assist.old` no lugar | a máquina caiu no meio da troca: `python3 ~/.config/herdr-app/mods/.herdr-assist.old/ctl.py status` restaura a pasta |
| `cmd-alt-a` não abre o dialog | o `keys.json` do mod não foi mesclado, ou a tecla já é de outro comando |
| Ponte não sobe: `hc binary not found` | `hc` fora do PATH: aponte `HC_BIN` |
| `limits claude [...]: claude auth: … --json` | a CLI do Claude instalada não aceita a flag; afeta só a tela opcional de limites, não os agentes |
