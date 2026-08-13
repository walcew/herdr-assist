# herdr-assist para M5Stack Cardputer

Porte do painel de 3,5" para o **M5Stack Cardputer** (StampS3): a mesma ponte, o
mesmo protocolo e a mesma configuração, numa tela de 240x135 operada por
teclado. O que o painel de mesa faz com o dedo, aqui se faz digitando — e é isso
que muda o proveito da coisa: **dá para responder o agente com texto livre**, não
só com as teclas da allowlist.

> Este diretório é um projeto PlatformIO independente. O firmware do painel
> continua na raiz do repositório (ESP-IDF + LVGL) e não é afetado.

| | |
|---|---|
| Placa | M5Stack Cardputer v1/v1.1 (ESP32-S3FN8, 8MB flash, **sem PSRAM**) |
| Tela | 1.14" 240x135, ST7789v2 |
| Teclado | 56 teclas, matriz varrida (a lib do M5 também cobre o Cardputer ADV) |
| Framework | Arduino + M5Cardputer/M5GFX |

## Compilando e gravando

```sh
cd cardputer
pio run                       # compila
pio run -t upload             # grava pela USB-C
pio device monitor            # log
```

O build gera `.pio/build/cardputer/firmware.bin` (aplicação, offset `0x10000`) e
`firmware.factory.bin` (bootloader + tabela + app, offset `0x0`).

**Pelo M5Launcher:** copie `firmware.bin` para o cartão microSD e escolha o
arquivo no launcher. Quem manda na partição nesse caso é a tabela que o launcher
já escreveu — o `partitions.csv` daqui só vale para gravação direta pela USB.

**Direto pelo esptool:**

```sh
esptool.py --chip esp32s3 --port COM5 write_flash 0x0 firmware.factory.bin
```

## Configurando

1. **Wi-Fi** — dois caminhos:

   **Pelo cartão (recomendado).** Crie `/herdr-wifi.txt` na raiz do microSD:

   ```ini
   ssid=MinhaRede
   pass=minhasenha
   ```

   No boot o firmware lê o arquivo e grava na NVS. O arquivo manda: se diverge
   do que está gravado, a NVS é regravada — para voltar a gerenciar pela tela,
   apague o arquivo. Trocar de rede vira editar um txt no computador, sem
   digitar senha no teclado de 56 teclas. A senha fica em texto puro no cartão,
   que é a mesma exposição de compilá-la no binário, com a vantagem de não
   entrar no repositório nem no `.bin` que circula.

   **Pela tela.** `SETTINGS` → `Wi-Fi` escolhe a rede na varredura e em seguida
   pede a senha; `Salvar e reiniciar` aplica. (A config só vale depois do
   restart, igual ao painel.) O campo de senha aparece **em texto claro** de
   propósito: num teclado que você está usando pela primeira vez, não poder
   conferir o que digitou custa mais que o risco de alguém ler por cima do
   ombro.

   Quando não há sessão para listar, a tela diz o motivo (`rede nao
   encontrada`, `senha recusada`, `conectando...`) — senha errada e roteador
   fora do ar não deviam parecer a mesma coisa.
2. **Ponte** — instale o plugin na máquina que você quer acompanhar
   (`herdr plugin install walcew/herdr-assist/plugin`), como descrito no
   [README principal](../README.md).
3. **Pareamento** — no aparelho: `SETTINGS` → `Parear com um host`. Ele mostra
   um código de 6 caracteres e se anuncia por broadcast UDP.

   No host, **macOS/Linux**: abra a tela de administração
   (`herdr plugin pane open --plugin herdr-assist --entrypoint admin`) e tecle
   `p`. No **Windows** (onde não há curses), pela linha de comando:

   ```sh
   python plugin/pair.py
   ```

   Nome, endereço, porta e token chegam prontos; o aparelho grava na NVS e
   reinicia já conectado. Nada é digitado no teclado do Cardputer.

O pareamento e a descoberta são exatamente os do painel — inclusive o HMAC do
`herdr-here` —, então um host pareado serve aos dois sem nada a mais.

## Teclado

Setas são `;` (cima), `.` (baixo), `,` (esquerda), `/` (direita), como manda a
serigrafia. `` ` `` é o esc e sempre sobe um nível.

A interface é **navegável**: tudo é alcançável com setas e enter, por menus. As
combinações abaixo existem como atalho para quem já pegou o jeito — nenhuma
delas é o único caminho até uma ação.

### Menus (raiz, sessões, dash, configurações)

| Tecla | O que faz |
|---|---|
| `;` `.` | move a seleção |
| `enter` | abre / edita / liga-desliga |
| `` ` `` | volta um nível |
| `,` `/` | atalho: percorre Sessões / Dash / Configurações |
| `x` | apaga o slot (só na tela de hosts) |

### Sessão aberta (terminal)

A regra é uma só: **digitar compõe uma linha**, que sai como texto no `enter`.
Com a linha vazia, `enter` abre o **menu de ações** — é ali que estão todas as
respostas ao agente, sem depender de combinação nenhuma.

| Tecla | O que faz |
|---|---|
| letras/números | entram na linha de digitação (rodapé) |
| `enter` (com texto) | manda a linha (`send_text`) e depois um Enter |
| `enter` (linha vazia) | abre o menu de ações |
| `del` | apaga um caractere; com a linha vazia, manda BSpace |
| `ctrl`+`;` `.` | rola o terminal (3 linhas) |
| `ctrl`+`,` `/` | rola uma tela inteira |
| `fn`+`;` `.` `,` `/` | setas para o agente |
| `fn`+`y` `n` `a` `0`-`9` | tecla crua imediata |
| `fn`+`enter` / `fn`+`` ` `` | Enter / Escape crus |
| `tab` | Tab |
| `ctrl`+`c` | interrompe (`C-c`) |
| `` ` `` | fecha a sessão (com a linha de digitação vazia) |

O menu de ações cobre tudo isso em lista: *Digitar resposta, Enviar Enter,
Responder sim/não, Aprovar, Escape, Interromper, Setas, Tab, Rolar, Focar no
host, Fechar sessão*.

**Rolagem.** O que a ponte entrega é o viewport do pane, não o histórico — a
tela trava o pane em 14 linhas e é isso que chega. Então rolar não é mover uma
janela local: é pedir ao host uma rolagem nativa (`scroll_pane`), uma roda de
mouse sobre a célula do meio da tela. A posição importa porque uma TUI só rola
a região sob o ponteiro.

**Por que não `shift` para rolar.** No Cardputer `shift+;` é como se digita
`:` — e `<`, `>`, `?` saem das outras três setas. Usar shift ali custaria esses
caracteres dentro do terminal.

## Versões

O fluxo de atualização é pelo cartão: o `.bin` vai para a raiz do microSD com a
versão no nome, e o M5Launcher instala o que você escolher. Manter a versão
anterior no cartão é de graça e serve de volta atrás.

| Versão | O que mudou |
|---|---|
| 1.0.0 | Sessão bloqueada pisca em vermelho; crédito na tela Sobre |
| 0.8.0 | Logo de boot no tamanho da tela, com traço suavizado (ver [docs/logo-boot.md](docs/logo-boot.md)) |
| 0.7.0 | Logo fiel ao original (preto sobre papel) e item "Ver a logo" no menu |
| 0.6.0 | Bateria na barra de status; logo de boot |
| 0.5.0 | Repintura incremental: a lista não pisca mais |
| 0.4.0 | Rolagem nativa via `scroll_pane`; setas reconhecidas com shift |
| 0.3.0 | Interface navegável: menu raiz e menu de ações na sessão |
| 0.2.0 | Wi-Fi provisionado por `/herdr-wifi.txt` no cartão; senha visível ao digitar; estado do Wi-Fi na tela quando não há sessão |
| 0.1.0 | Primeira versão: sessões, terminal com teclado, dash de limites, pareamento, configurações |

Para gravar pela USB **sem perder o M5Launcher**, escreva só o app no slot onde
ele instalou o firmware (o launcher reparticiona: no meu caso `app0` é o próprio
launcher e `app1`, em `0x200000`, é o herdr-assist). Confira antes:

```sh
python -m esptool --port COM7 --chip esp32s3 read-flash 0x8000 0xc00 ptable.bin
python -m esptool --port COM7 --chip esp32s3 write-flash 0x200000 .pio/build/cardputer/firmware.bin
```

Um `pio run -t upload` cru escreveria a tabela de partições deste projeto em
`0x8000` e o app em `0x10000` — apagando o launcher.

## O que este porte não tem (ainda)

Comparado ao painel de 3,5": sem mascote/avatar, sem tela home com relógio
grande e mapa de calor, sem atualização OTA pelo GitHub e sem tradução (a
interface é curta e está em português).

No host **Windows** não há tela de administração: o `admin.py` é curses, que não
existe na stdlib do Windows. O pareamento está coberto por `plugin/pair.py`, mas
rotação de token, status da ponte e instalação do atalho de teclado seguem só no
macOS e no Linux.

## Notas de implementação

- **Reuso.** `sync_shared.py` copia de `../src` para `src/shared/` a cada build o
  que não pode divergir entre os dois firmwares: `panel_cfg`, `herdr_model`,
  `herdr_conn`, `pairing`, `discovery` e `term_parse`. Nada disso tem LVGL, e
  compila igual no Arduino. `src/shared/` é gerado — edite `../src`.
- **Contrato de Wi-Fi.** O único módulo do painel que foi reescrito é o `net.h`:
  `src/net.cpp` o implementa com a lib WiFi do Arduino. O `herdr_conn.c`
  compartilhado não vê diferença.
- **Sem PSRAM.** O S3FN8 não tem PSRAM, então os buffers do painel são
  reduzidos pelo build (`TERM_*`, `HERDR_RX_BUF_LEN`) e o grid do terminal
  (~20KB) só é alocado enquanto a sessão está aberta. Não há buffer da tela
  inteira: o antiflicker é um sprite de uma linha empurrado com clip.
- **Geometria.** A tela cabe 40x14 células de 6x8 (ou 60x19 com a fonte de 4x6,
  em `SETTINGS` → `Terminal`). Essa geometria vai no `read_pane`, e a ponte
  trava o pane do host nesse tamanho enquanto a sessão está aberta — o
  `release_pane` no `` ` `` devolve.
- **Fora do ASCII.** A fonte de 6x8 é ASCII puro, então box-drawing, ✓, ●,
  acentos e afins são reduzidos a um vizinho visual, sempre um caractere por
  coluna para o alinhamento com o host não se perder.
