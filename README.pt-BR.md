# herdr-assist

> 🇺🇸 **This document is also available in English:** [README.md](README.md)
>
> ⚠️ O idioma principal do projeto é o inglês. Esta versão é mantida em paralelo, mas
> em caso de divergência o [README.md](README.md) é a referência.

Painel físico dedicado para monitorar e controlar sessões do [Herdr](https://herdr.dev) —
o multiplexador de terminal para agentes de código. Roda num kit ESP32-S3 com display
touch de 3.5", ficando na mesa ao lado do teclado: os agentes aparecem com o estado em
cores, e quando um deles para para pedir uma decisão o painel toca um sino que abre a
sessão certa com um toque, sem precisar procurar a janela no terminal.

| Home | Sessões | Dashboards |
|:---:|:---:|:---:|
| ![Tela inicial: relógio, mascote e mapa de calor por host](docs/images/panel-home.jpg) | ![Lista de sessões agrupada por host](docs/images/panel-sessions.jpg) | ![Limites de uso dos provedores](docs/images/panel-dash.jpg) |
| Relógio, mascote e um mapa de calor por host | Sessões com estado, agrupadas por host | Limites de uso do Claude e do Codex |

## Começando

Três passos: gravar o painel, instalar a ponte na máquina que você quer controlar e
parear os dois. **Sem toolchain e sem compilar** — o release traz os binários prontos.

### 1. Gravando o painel

Baixe os binários da
[página de releases](https://github.com/walcew/herdr-assist/releases/latest), ligue a
placa na USB e grave a imagem:

```sh
# o esptool é o único requisito
pipx install esptool          # ou: brew install esptool

# descubra a porta — ela muda conforme a entrada USB usada
esptool.py chip_id            # macOS: /dev/cu.usbmodemXXXX, Linux: /dev/ttyACM0

esptool.py --chip esp32s3 --port /dev/cu.usbmodemXXXX \
    write_flash 0x0 herdr-assist-v0.1.0-install.bin
```

O painel reinicia na tela de configurações: escolha sua rede Wi-Fi na lista e digite a
senha. Deixe os hosts em branco por enquanto — o passo 3 preenche para você.

> **Instalar × atualizar.** A imagem `-install.bin` cobre de `0x0` a `0x10000`, faixa que
> inclui a partição NVS, então ela **apaga a configuração do painel** (Wi-Fi, hosts,
> tokens, idioma). É o que se quer numa placa nova. Para atualizar um painel já
> configurado, grave só o app e preserve tudo:
>
> ```sh
> esptool.py --chip esp32s3 --port /dev/cu.usbmodemXXXX \
>     write_flash 0x10000 herdr-assist-v0.1.0-update.bin
> ```

### 2. Instalando a ponte no host

A ponte é um plugin do Herdr. Rode em cada máquina que você quiser alcançar pelo painel:

```sh
git clone https://github.com/walcew/herdr-assist.git
herdr plugin link herdr-assist/plugin   # registra e habilita
```

O Herdr sobe a ponte junto com a sessão a partir do próximo boot. Para subir agora sem
reiniciar, rode a ação `Restart the bridge` do plugin. Só stdlib do Python — funciona com
o `python3` de fábrica do macOS (3.9+), sem toolchain.

### 3. Pareando o painel

Digitar 32 caracteres hexadecimais num touch de 3,5" é inviável, então o sentido da
configuração se inverte: **o host manda a configuração pronta para o painel**.

1. No painel: **Settings → Pair with a host**. A tela mostra um código de 6 caracteres e
   passa a se anunciar por broadcast UDP por 3 minutos.
2. No host, de dentro de qualquer pane do Herdr, abra a tela de administração e tecle `p`:
   ```sh
   herdr plugin pane open --plugin herdr-assist --entrypoint admin
   ```
3. Escolha na lista o código que aparece na tela do painel.

O host envia nome, endereço, porta e token; o painel grava na NVS e reinicia já
conectado. Nada é digitado no touch.

Pronto — o painel já lista suas sessões. O resto deste documento é referência.

## Idioma

A interface fala **inglês e português**, e o firmware é o mesmo nos dois casos: o idioma
sai da NVS (**Settings → Device → Language**, que alterna no toque) e vale a partir do
reinício, junto com o resto da config. De fábrica o painel liga em inglês. A tela de
administração no host segue o locale da máquina (`LANG` e afins, ou `HERDR_ASSIST_LANG`
para forçar).

## Hardware

**JC3248W535EN** (Guition/Sunton), ~R$ 120 no AliExpress:

| Componente | Especificação |
|---|---|
| MCU | ESP32-S3-WROOM-1 (240 MHz, 2 cores) |
| Memória | 8 MB PSRAM (OPI) + 16 MB flash |
| Display | 3.5" IPS 320×480, controlador AXS15231B via QSPI |
| Touch | Capacitivo, mesmo AXS15231B, via I2C (SCL 8 / SDA 4) |
| Rede | Wi-Fi 2.4 GHz + BLE |

Pinout relevante em `src/esp_bsp.h`; esquemáticos e datasheets em `docs/`.

## Arquitetura

O Herdr expõe sua API num unix socket local, que um dispositivo na rede não alcança.
A ponte (`plugin/herdr_bridge.py`, stdlib pura) traduz isso para TCP e nada mais: ela
assina os eventos do Herdr, então o painel recebe as mudanças por **push**, sem polling.
Ela é empacotada como **plugin do Herdr** (`plugin/herdr-plugin.toml`), que o Herdr
sobe junto com a sessão — um plugin por máquina que você queira controlar pelo painel.

```
┌─ JC3248W535EN ─────────────┐     Wi-Fi / LAN      ┌─ Mac ───────────────────────┐
│ herdr-assist               │ ◄── TCP + JSON ───►  │ plugin herdr-assist (:9375) │
│ ESP-IDF 5.2 + LVGL 8.4     │    uma msg por       │          │ unix socket      │
│ UI: lista, terminal, ações │    linha             │          ▼                  │
└────────────────────────────┘                      │   herdr (events.subscribe)  │
                                                    └─────────────────────────────┘
```

**Painel → ponte:** `hello` (token, obrigatório na 1ª linha), depois `read_pane`
(leva também a geometria da tela do painel), `send_keys`, `send_text`, `focus`,
`scroll_pane`, `release_pane`, `ping`.
**Ponte → painel:** `agents` (estado de todos, incluindo quem está `blocked`),
`pane_content` (tela do terminal **com SGR/cores**, fiel ao host —
só linhas em branco do fim são removidas; quem emula é o motor do Ghostty embutido no
Herdr, via `pane.read format:"ansi"`, e o painel só parseia SGR e pinta), `limits`
(uso de limites dos provedores de IA para a aba Dash — coletado dos endpoints de uso
do Claude Code e do Codex com as credenciais que os próprios CLIs mantêm renovadas em
`~/.claude/.credentials.json` e `~/.codex/auth.json`; nenhum token sai do Mac, só
percentuais), `pong`.

A ponte é o ponto onde as decisões de segurança acontecem, porque qualquer um na LAN
alcança essa porta. **Toda conexão exige um token** (handshake `hello` na primeira linha;
sem ele, a ponte desconecta em 5 s). Além disso: só teclas de uma allowlist passam, texto
tem limite de tamanho, e comandos só valem para panes que existem. O token é gerado na
primeira subida do plugin (0600 no config-dir); a ação `Show panel token`, no Herdr,
exibe o valor.

## Usando a tela de administração

Pela linha de comando, de dentro de qualquer pane do Herdr:

```sh
herdr plugin pane open --plugin herdr-assist --entrypoint admin   # abre
herdr plugin pane close --plugin herdr-assist --entrypoint admin  # ou tecle "q"
```

A tela abre em overlay sobre o pane ativo. Dentro dela: `p` pareia um painel, `r` gira o
token, `x` reinicia a ponte, `q` fecha.

> Por SSH o `herdr` pode não estar no PATH (uma sessão não-interativa não carrega o
> ambiente do Homebrew): use o caminho completo, normalmente `/opt/homebrew/bin/herdr`.

Vale ligar um atalho, porque o Herdr 0.8.0 **não lista ações de plugin em menu nenhum** —
os itens do menu de contexto são fixos, e o campo `contexts` do manifest só declara em que
contextos a ação é válida. Keybinding e CLI são as vias de uso:

```toml
[[keys.command]]
key = "prefix+a"          # ctrl+b seguido de "a"
type = "plugin_action"
command = "herdr-assist.admin"
```

`herdr config check` valida e `herdr server reload-config` aplica sem reiniciar.

A mesma tela de administração mostra o estado da ponte, o token e os painéis conectados.
Pela CLI, o token também sai em:

```sh
cat "$(herdr plugin config-dir herdr-assist)/token"
```

O log da ponte fica em `<state-dir>/bridge.log` — o Herdr usa
`~/.local/state/herdr/plugins/<id>/`, que é diferente do config-dir.

> O plugin também aceita config opcional em `<config-dir>/env` (`BRIDGE_PORT`,
> `BRIDGE_BIND`) — o config-dir sai em `herdr plugin config-dir herdr-assist`.

## Compilando do código-fonte

Só é preciso se você for mexer no firmware — para apenas usar o painel, veja
[Começando](#começando).

Requer [PlatformIO](https://platformio.org/) (`pipx install platformio`). A toolchain
ESP-IDF é baixada sozinha na primeira build (~1 GB).

```sh
pio run                                        # compila
pio device list                                # a porta muda conforme a entrada USB
pio run -t upload --upload-port /dev/cu.usbmodemXXXX
```

O firmware é genérico — nenhuma credencial é compilada. No primeiro boot (ou após
apagar a NVS) o painel abre a tela de configurações: escolha a rede Wi-Fi na lista,
digite a senha e cadastre até 4 hosts Herdr (nome, IP ou hostname, porta e **token**
da ponte). Tudo fica na NVS e sobrevive a reflashes do app; salvar reinicia o painel.

Para regenerar a fonte do terminal (só é preciso ao mudar os ranges de glifos):

```sh
./scripts/gen_font.sh
```

### Publicando um release

Empurre uma tag `v*` e [o workflow](.github/workflows/release.yml) compila, mescla a
imagem de instalação e publica o release com os dois binários e seus checksums:

```sh
git tag -a v0.1.0 -m "v0.1.0"
git push origin v0.1.0
```

## Estrutura

| Arquivo | Papel |
|---|---|
| `plugin/herdr-plugin.toml` | Manifest do plugin do Herdr (startup, pane de administração, ações) |
| `plugin/start.sh` | Startup do plugin: sobe a ponte destacada, idempotente, gera token |
| `plugin/herdr_bridge.py` | Ponte: socket do Herdr ↔ TCP, handshake com token, allowlist, sanitização, coleta de uso de limites (Claude/Codex) |
| `plugin/admin.py` | Tela de administração no Herdr: status, token, pareamento, girar token |
| `plugin/i18n.py` | Textos da tela de administração (en/pt), idioma vindo do locale do host |
| `src/pairing.c` | Modo de pareamento do painel: anúncio por broadcast + recepção da config |
| `src/DEMO_LVGL.c` | Ponto de entrada: inicializa painel, config, Wi-Fi, conexões e UI |
| `src/panel_cfg.c` | Configuração persistente (NVS): rede Wi-Fi + até 4 hosts Herdr + idioma |
| `src/net.c` | Wi-Fi station com reconexão automática e scan para a tela de config |
| `src/herdr_conn.c` | Uma conexão TCP por host, parse do protocolo, ping e reconexão |
| `src/herdr_model.c` | Estado compartilhado entre as tasks de rede e a da UI (mutex + geração) |
| `src/term_parse.c` | Parser SGR puro (sem LVGL/ESP): snapshot ANSI → grid de runs coloridos; testável no Mac (`scripts/term_parse_test.c`) |
| `src/term_view.c` | Widget custom da tela de terminal: desenha o grid com cor/estilo por run, pan horizontal fiel |
| `src/i18n.c` | Textos da interface (en/pt) numa lista só, que gera enum e tabela |
| `src/ui_theme.c` | Paleta, fontes, topbar e dock compartilhados pelas telas |
| `src/herdr_ui.c` | UI LVGL: home (relógio, mascote, resumo, mapa de calor), sessões, dash de limites, terminal, ações, teclado |
| `src/herdr_ui_settings.c` | Aba de configurações: scan de Wi-Fi, senha, editor de hosts, idioma |
| `src/lv_font_terminal_12.c` | Fonte gerada (não editar) — veja `scripts/gen_font.sh` |
| `src/esp_bsp.c`, `src/esp_lcd_axs15231b.c`, `src/lv_port.c` | BSP do fabricante (display, touch, port LVGL) |

### Notas de implementação

- **`full_refresh = 1`** (`lv_port.c`) não é escolha estética, e sai caro: são 307 KB por
  quadro no barramento QSPI mesmo quando muda uma linha de texto. Duas coisas impedem a
  renderização parcial neste painel. A primeira é conhecida: o driver pulava o RASET (0x2B)
  em QSPI ([esp-bsp#724](https://github.com/espressif/esp-bsp/issues/724)) — já corrigido
  aqui. A segunda não está no issue e foi encontrada na marra: `draw_bitmap` usa **RAMWRC**
  (0x3C, "continua a escrita anterior") sempre que `y_start != 0`, ignorando a janela que
  acabou de definir. Funciona para varredura sequencial, que é o padrão do refresh completo;
  com áreas arbitrárias, cada região vai parar no lugar errado e a tela embaralha. Habilitar
  parcial exige também trocar esse RAMWRC por RAMWR — não testado.
- **Rotação é sempre por software.** O painel ignora MADCTL, então rotação em runtime não
  funciona em nenhuma stack; `LVGL_PORT_ROTATION_DEGREE` em `DEMO_LVGL.c` resolve em tempo
  de compilação. O app usa 0° (portrait nativo 320×480), que dispensa a cópia de rotação
  no flush — se a imagem ficar de cabeça para baixo na sua base, use 180°.
- **O touch é capacitivo e não precisa de calibração** — o que importa é o remapeamento de
  coordenadas conforme a rotação, feito em `lv_port.c`.
- **Fontes**: nenhuma fonte que acompanha a LVGL serve aqui — Montserrat e unscii cobrem
  apenas ASCII 0x20–0x7F, então box-drawing e spinners viram retângulos vazios, e rótulos
  como "Sessões" e "Endereço" perdem os acentos. São duas famílias geradas: o terminal usa
  a JetBrainsMono Nerd inteira (`scripts/gen_font.sh`) e a interface usa a Montserrat com o
  Latin-1 completo, mesclada com a FontAwesome da própria LVGL para os `LV_SYMBOL_*`
  continuarem valendo (`scripts/gen_font_ui.sh`), mais pontuação tipográfica/setas da
  própria Montserrat e `✓ ✗ ⚠ ●` da DejaVu Sans — símbolos que chegam aos alertas em
  texto vindo do terminal. A Nerd Font, mesmo inteira, não cobre tudo que o Claude Code
  desenha (spinner `✢✳✻✽`, `⏺ ⏵ ⏸ ⏳`, `◑ ◼ ✅ ✔ ⧉ ※` — auditado contra o cmap):
  esses viram vizinhos visuais que a fonte tem (`⏺→●`, `✳→✶`, `✅→✓`...) na tabela
  `GLYPH_SWAPS` de `replace_missing_glyphs()` — trocar por outra fonte quebraria o
  grid de 7 px do term_view, que assume avanço uniforme.
- **O idioma é decidido no boot e não muda em runtime.** As telas da LVGL são montadas
  uma vez em `herdr_ui_init()` e ficam vivas escondidas — retraduzir exigiria destruir e
  reconstruir todas elas, ou guardar a chave de cada label. Como trocar de rede ou de host
  já reinicia o painel, o idioma entrou no mesmo caminho: a tela de configurações edita a
  cópia, o toast avisa que falta aplicar, e o reinício resolve. `i18n.h` tem uma lista
  X-macro única (`I18N_STRINGS`) de onde saem tanto o enum das chaves quanto a tabela de
  traduções, então não existe o caso de acrescentar um texto num idioma e esquecer o
  outro — as duas traduções ficam na mesma linha. As tabelas são `const char *const`:
  vivem em flash (3,2 KB de rodata para os dois idiomas juntos, medido no `.o`), e o
  único custo em RAM é o `uint8_t` do idioma em vigor. pt_BR e en_US
  cabem no Latin-1 que as fontes de UI já cobrem, então nada precisou ser regerado.
  Ficam fora do bilíngue, em inglês fixo, o `herdr-plugin.toml` (o manifest é lido pelo
  Herdr sem campo por idioma) e as duas mensagens do `start.sh` que a tela de
  administração ecoa ao reiniciar a ponte — um shell script não acompanha o locale sem
  carregar uma tabela só para isso, e inglês é o mesmo critério do manifest.
- **A interface segue o projeto "herdr-assist" no Claude Design** — paleta neutra (cor só
  em status), topbar sem barra sólida e dock flutuante de quatro abas. Ao mexer no visual,
  atualize lá também: `src/ui_theme.h` é a tradução direta daquelas telas.
- **A tela de terminal renderiza a formatação real** (`term_parse.c` + `term_view.c`): a
  ponte pede `pane.read` com `format:"ansi"` — o emulador interno do Herdr é o libghostty,
  que re-emite a tela como linhas com SGR (só SGR: sem cursor/OSC, confirmado em amostra
  real) — e o painel parseia isso num grid de runs e desenha com `lv_draw_rect`/`lv_draw_label`
  em célula de 7×19 px (métricas da `lv_font_terminal_12`). Sem re-wrap: o grid fica na
  largura real do pane no host, com âncora vertical no fim e pan preservado entre refreshes —
  o pan horizontal continua existindo como rede para quando a trava de resolução (abaixo) não
  pega. Caps: 220 colunas × 48 linhas × 12 KB (`HERDR_CONTENT_LEN`,
  casado com o cap de 12000 da ponte). Limitações assumidas: itálico não tem efeito visual
  (fonte bpp1 única), bold vira cor clareada (bright na paleta indexada, +30% branco em
  truecolor), e emoji vira `"* "` (2 células, preservando alinhamento). Snapshot idêntico
  não repinta: o dedup por `seq` no model evita o full refresh de 307 KB à toa.
- **Sessão aberta no painel roda na resolução da tela do painel.** Herdar as ~135 colunas do
  host obrigava a arrastar a tela para o lado para ler qualquer coisa, então o `read_pane`
  leva junto a geometria que cabe (`term_view_fit()`: 42 × 18) e a ponte trava o pane nesse
  tamanho enquanto o painel estiver lendo. A API JSON do Herdr não tem tamanho de terminal
  (`pane.resize` é proporção de split), então quem faz isso é a CLI
  `herdr terminal session control <pane_id> --cols N --rows M`, que redimensiona o pty de
  verdade (TIOCSWINSZ) e mantém o tamanho travado enquanto o processo viver. A soltura tem
  três caminhos independentes: `release_pane` ao fechar o detalhe, desconexão do painel, e
  morte da ponte — neste último o filho vê EOF no stdin e se desanexa sozinho, e é por isso
  que o stdin dele é um pipe. Consequência aceita: enquanto travado, esse pane também
  aparece estreito no Herdr do Mac, e cada abertura/fechamento faz o agente reflowar.
- **A rolagem do terminal é a do host, não a do widget.** Arrastar na vertical vira
  `scroll_pane` → `terminal.scroll` com `source:"wheel"` no controller, e daí quem decide é
  o Herdr: app com mouse tracking (Claude Code) recebe o evento e rola o próprio conteúdo,
  senão anda o scrollback do emulador. Por isso o `pane.read` usa `source:"visible"` — é o
  viewport que a rolagem move; com `"recent"` o painel ficaria preso no fim. Duas coisas que
  custaram medição: a **posição** do ponteiro vai junto (célula sob o dedo — com a roda fora
  da área de transcript o Claude Code simplesmente ignora), e uma célula arrastada equivale
  a uma linha. O eixo vertical do LVGL só é liberado quando o conteúdo não cabe
  (`term_view_set_ansi` alterna `LV_DIR_HOR`/`LV_DIR_ALL`), caso em que a trava de resolução
  falhou e rolar localmente é o certo.
- **Decisão pendente é um beacon, não um formulário reconstruído.** Quando algum agente
  entra em `blocked`, um sino vermelho aparece na home — na linha do relógio, do lado
  oposto — balançando até alguém agir; o toque abre a sessão que está esperando (com o
  número de sessões no rótulo quando é mais de uma). Antes disso, a ponte tentava
  reconhecer o formulário por regex, mandava a pergunta e as opções detectadas, e o
  painel montava um modal com um botão por opção; responder fazia a ponte navegar o
  cursor por setas e confirmar. Isso saiu inteiro: com o terminal do painel já rodando na
  resolução da tela e com rolagem, ver a pergunta como ela é e responder com ↑/↓/Enter é
  mais fiel do que adivinhá-la — e some a classe de erro em que o botão não correspondia
  ao que estava na tela. O que sustenta o beacon é o `agent_status` que o `agents` já
  trazia. Consequência assumida: o alerta só existe na home (o modal aparecia sobre
  qualquer tela).
- **O relógio da home depende de SNTP** (`net.c`), com fuso fixo em UTC-3. Sem sincronizar,
  a home mostra `--:--` em vez de uma hora inventada.
- **Detecção de conexão morta** (`herdr_conn.c`): com push, silêncio é o estado normal, então
  não dá para inferir queda pela ausência de dados. O painel manda `ping` a cada 20 s e
  desiste da conexão após 50 s sem resposta — isso cobre o caso em que o TCP fica aberto mas
  para de receber (Wi-Fi que some, Mac que dorme), que de outra forma trava o painel para
  sempre em "conectando".
- **Buffers grandes ficam em memória estática, nunca na pilha**: a task da LVGL tem 8 KB e a
  de rede 6 KB, enquanto uma leitura de terminal sozinha passa de 8 KB. Declarar essas
  structs como variável local estoura a pilha e derruba o device com backtrace corrompido.

## Créditos e licença

O BSP, os drivers de display/touch e o port da LVGL vêm do
[port PlatformIO de NorthernMan54](https://github.com/NorthernMan54/JC3248W535EN),
que empacota o material original do fabricante (Shenzhen Jingcai / Guition). A LVGL 8.4
está vendorizada em `libraries/lvgl`, podada dos demos e exemplos que não entram no build.

O desenho do protocolo (os tipos `agents` / `pane_content`, e o `blocked` com as opções de
aprovação detectadas, que existiu aqui até o beacon substituí-lo) veio do relay do
[herdr-remote](https://github.com/dcolinmorgan/herdr-remote), usado como referência antes de
trocarmos por uma ponte própria falando o socket nativo do Herdr.

MIT — veja `LICENSE`.
