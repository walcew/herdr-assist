# Ponte do herdr-assist

Processo que traduz o socket do Herdr para o TCP autenticado que o painel (e o pager
e-paper) consomem. Python puro, só stdlib (3.9+), sem toolchain.

## Subir agora, sem instalar nada

```sh
cd mod/herdr-assist
python3 start.py
```

`start.py` destaca o processo e sai — pode fechar o terminal. Ele é **idempotente**: se a
porta já responde, imprime `bridge already running on port 9375` e termina com sucesso, que
é o que permite chamá-lo de um hook de startup sem risco de subir duas pontes.

```sh
python3 start.py --restart    # derruba quem estiver na porta e sobe de novo
```

Para depurar em primeiro plano (log na tela, Ctrl+C encerra):

```sh
python3 herdr_bridge.py
```

## Permanente (plugin do Herdr)

```sh
herdr plugin install walcew/herdr-assist/plugin     # do repositório publicado
```

ou, com um checkout local:

```sh
herdr plugin link /caminho/para/herdr-assist/plugin
herdr plugin action invoke herdr-assist.restart-bridge
```

O `[[startup]]` do manifesto roda `start.py` a cada sessão do Herdr; o `restart-bridge` só
evita esperar a próxima. `herdr plugin list` mostra se está habilitado.

## Onde ficam as coisas

| | Instalado como plugin | Rodando avulso |
|---|---|---|
| Config (`token`, `env`) | `herdr plugin config-dir herdr-assist` → `~/.config/herdr/plugins/config/herdr-assist/` | `~/.config/herdr-assist/` |
| Estado (`bridge.pid`, `bridge.log`) | `~/.local/state/herdr/plugins/herdr-assist/` | mesmo diretório da config |

> **Trocando de avulso para plugin, copie o `token` e o `env`.** O plugin recebe um
> diretório de config próprio: sem o token lá, a ponte gera um novo e todo painel já
> pareado deixa de conectar (o sintoma é o painel em `OFFLINE` e nada no log da ponte).

## Configuração — `env`, uma variável por linha

```sh
BRIDGE_PORT=9375        # porta TCP (painéis) e UDP (descoberta)
BRIDGE_BIND=0.0.0.0     # 0.0.0.0 é o que faz a descoberta por broadcast funcionar
BRIDGE_TOKEN=...        # opcional; sem ela vale o arquivo `token`
```

> A ponte nova lê token, porta, repositórios de avatar e atualização automática do doc
> `herdr-assist` do core (migrando o `token` e o `env` daqui na primeira subida), e
> `BRIDGE_MACHINES` não existe mais. Esta seção descreve o plugin do Herdr TUI.

## Firewall

A máquina da ponte precisa aceitar **TCP e UDP** na porta (9375 por padrão), na entrada:
TCP é a conexão dos painéis, UDP é a descoberta por broadcast. No Windows com WSL o
firewall comum não governa isso — o Hyper-V firewall bloqueia entrada por padrão:

```powershell
New-NetFirewallHyperVRule -Name "herdr-assist"     -Direction Inbound -Protocol TCP -LocalPorts 9375 -Action Allow
New-NetFirewallHyperVRule -Name "herdr-assist-udp" -Direction Inbound -Protocol UDP -LocalPorts 9375 -Action Allow
```

Sem a regra UDP a ponte sobe, escuta, e o painel fica em `OFFLINE` sem erro nenhum do lado
da ponte. Sem a TCP ele encontra e não conecta.

## Ver o que está acontecendo

```sh
ss -ltnp | grep 9375                       # está ouvindo?
tail -f <estado>/bridge.log                # o que ela está fazendo
python3 ctl.py status                      # Ponte, porta, painéis e quem escuta a porta (JSON)
python3 ctl.py discover                    # painéis em modo de pareamento na LAN (JSON)
herdr plugin log list                      # logs das ações do plugin, do lado do Herdr
```

Uma ponte saudável loga, em ordem: `ponte versão …`, `descoberta udp em …`, `ponte ouvindo
em 0.0.0.0:9375`, `assinado: N genéricos + M panes` e, a cada painel que entra,
`painel conectado: ('<ip>', <porta>)`.

## Problemas comuns

| Sintoma | Causa provável |
|---|---|
| `bridge already running` mas o painel não conecta | token trocado (ver a nota do diretório de config) ou porta bloqueada no firewall |
| Painel em `OFFLINE` e log sem erro | falta a regra **UDP**; a descoberta não chega |
| `socket do Herdr não encontrado` | rodando fora do plugin sem `HERDR_SOCK`/`HERDR_SOCKET_PATH` |
| `limites claude: claude auth: … --json` | a CLI do Claude instalada não aceita a flag; afeta só a tela opcional de limites, não os agentes |
