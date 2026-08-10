# herdr-assist

Painel físico dedicado para monitorar e controlar sessões do [Herdr](https://herdr.dev) —
o multiplexador de terminal para agentes de código. Roda num kit ESP32-S3 com display
touch de 3.5", ficando na mesa ao lado do teclado: os agentes aparecem com o estado em
cores, e as aprovações que travam o trabalho podem ser respondidas com um toque, sem
precisar procurar a janela certa no terminal.

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

**Painel → ponte:** `hello` (token, obrigatório na 1ª linha), depois `read_pane`,
`send_keys`, `send_text`, `respond`, `focus`, `ping`.
**Ponte → painel:** `agents` (estado de todos), `blocked` (aprovação pendente com as
opções já detectadas), `pane_content` (saída do terminal, limpa de spinners), `pong`.

A ponte é o ponto onde as decisões de segurança acontecem, porque qualquer um na LAN
alcança essa porta. **Toda conexão exige um token** (handshake `hello` na primeira linha;
sem ele, a ponte desconecta em 5 s). Além disso: só teclas de uma allowlist passam, texto
tem limite de tamanho, e comandos só valem para panes que existem. O token é gerado na
primeira subida do plugin (0600 no config-dir); a ação `Mostrar token do painel`, no
Herdr, exibe o valor para você cadastrar no painel.

## Compilando

Requer [PlatformIO](https://platformio.org/) (`pipx install platformio`). A toolchain
ESP-IDF é baixada sozinha na primeira build (~1 GB).

```bash
pio run                                        # compila
pio device list                                # a porta muda conforme a entrada USB
pio run -t upload --upload-port /dev/cu.usbmodemXXXX
```

O firmware é genérico — nenhuma credencial é compilada. No primeiro boot (ou após
apagar a NVS) o painel abre a tela de configurações: escolha a rede Wi-Fi na lista,
digite a senha e cadastre até 4 hosts Herdr (nome, IP ou hostname, porta e **token**
da ponte). Tudo fica na NVS e sobrevive a reflashes do app; salvar reinicia o painel.

### Instalando a ponte num host novo

A ponte é um plugin do Herdr. Em cada máquina que você quiser controlar pelo painel:

```bash
git clone git@github.com:walcew/herdr-assist.git
herdr plugin link herdr-assist/firmware/plugin   # registra e habilita
```

O Herdr sobe a ponte junto com a sessão a partir do próximo boot; para subir agora sem
reiniciar, rode a ação `Reiniciar a ponte` do plugin. Só stdlib do Python — funciona com
o `python3` de fábrica do macOS (3.9+), sem toolchain. Pegue o token com a ação
`Mostrar token do painel` e cadastre-o no host correspondente na tela de configurações.

> Instalar direto de repositório privado com `herdr plugin install owner/repo/subdir`
> não funciona (ele baixa um tarball público); use o `git clone` + `herdr plugin link`
> acima. O plugin também aceita config opcional em `<config-dir>/env` (`BRIDGE_PORT`,
> `BRIDGE_BIND`) — o config-dir sai em `herdr plugin config-dir herdr-assist`.

Para regenerar a fonte do terminal (só é preciso ao mudar os ranges de glifos):

```bash
./scripts/gen_font.sh
```

## Estrutura

| Arquivo | Papel |
|---|---|
| `plugin/herdr-plugin.toml` | Manifest do plugin do Herdr (startup, ações show/restart) |
| `plugin/start.sh` | Startup do plugin: sobe a ponte destacada, idempotente, gera token |
| `plugin/herdr_bridge.py` | Ponte: socket do Herdr ↔ TCP, handshake com token, allowlist, sanitização |
| `src/DEMO_LVGL.c` | Ponto de entrada: inicializa painel, config, Wi-Fi, conexões e UI |
| `src/panel_cfg.c` | Configuração persistente (NVS): rede Wi-Fi + até 4 hosts Herdr |
| `src/net.c` | Wi-Fi station com reconexão automática e scan para a tela de config |
| `src/herdr_conn.c` | Uma conexão TCP por host, parse do protocolo, ping e reconexão |
| `src/herdr_model.c` | Estado compartilhado entre as tasks de rede e a da UI (mutex + geração) |
| `src/ui_theme.c` | Paleta, fontes, topbar e dock compartilhados pelas telas |
| `src/herdr_ui.c` | UI LVGL: home (relógio, mascote, resumo, mapa de calor), sessões, terminal, ações, teclado |
| `src/herdr_ui_settings.c` | Aba de configurações: scan de Wi-Fi, senha, editor de hosts |
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
  continuarem valendo (`scripts/gen_font_ui.sh`). O `✳` (U+2733) que o Claude usa nos
  títulos não existe nem na Nerd Font, e é trocado por `*` em `replace_missing_glyphs()`.
- **A interface segue o projeto "herdr-assist" no Claude Design** — paleta neutra (cor só
  em status), topbar sem barra sólida e dock flutuante de três abas. Ao mexer no visual,
  atualize lá também: `src/ui_theme.h` é a tradução direta daquelas telas.
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

O desenho do protocolo (os tipos `agents` / `blocked` / `pane_content` e a ideia de detectar
as opções de aprovação para virar botões) veio do relay do
[herdr-remote](https://github.com/dcolinmorgan/herdr-remote), usado como referência antes de
trocarmos por uma ponte própria falando o socket nativo do Herdr.

MIT — veja `LICENSE`.
