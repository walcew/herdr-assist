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

O Herdr expõe sua API por um unix socket local, que um dispositivo na rede não alcança.
A ponte é o relay do [herdr-remote](https://github.com/dcolinmorgan/herdr-remote), que já
resolve isso para clientes web/mobile — este firmware é mais um cliente do mesmo protocolo.

```
┌─ JC3248W535EN ─────────────┐      Wi-Fi / LAN      ┌─ Mac ──────────────────────┐
│ herdr-assist               │ ◄─── WebSocket ────►  │ herdr-remote relay (:8375) │
│ ESP-IDF 5.2 + LVGL 8.4     │    JSON: agents,      │          │                 │
│ UI: lista, terminal, ações │    blocked, keys...   │          ▼                 │
└────────────────────────────┘                       │      herdr CLI             │
                                                     └────────────────────────────┘
```

**Relay → device:** `agents` (snapshot a cada 2 s), `blocked` (aprovação pendente),
`pane_content` (saída do terminal).
**Device → relay:** `read_pane`, `send_keys`, `send_text`, `respond`, `focus`.

> O tipo `focus` é uma extensão nossa ao relay — veja `patches/`.

## Compilando

Requer [PlatformIO](https://platformio.org/) (`pipx install platformio`). A toolchain
ESP-IDF é baixada sozinha na primeira build (~1 GB).

```bash
cp src/wifi_creds.h.example src/wifi_creds.h   # preencha SSID, senha e IP do relay
pio run                                        # compila
pio run -t upload --upload-port /dev/cu.usbmodem101
```

`src/wifi_creds.h` é gitignorado — as credenciais nunca entram no versionamento.

Do lado do Mac, o relay precisa estar rodando:

```bash
cd herdr-remote && uv run relay/herdr_relay.py   # ou relay/install-service.sh p/ launchd
```

## Estrutura

| Arquivo | Papel |
|---|---|
| `src/DEMO_LVGL.c` | Ponto de entrada: inicializa painel, Wi-Fi, WS e UI |
| `src/net.c` | Wi-Fi station com reconexão automática |
| `src/herdr_ws.c` | Cliente WebSocket, parse do protocolo, supervisor de conexão |
| `src/herdr_model.c` | Estado compartilhado entre a task de rede e a da UI (mutex + geração) |
| `src/herdr_ui.c` | UI LVGL: lista de agentes, terminal, ações, teclado |
| `src/esp_bsp.c`, `src/esp_lcd_axs15231b.c`, `src/lv_port.c` | BSP do fabricante (display, touch, port LVGL) |

### Notas de implementação

- **`full_refresh = 1`** (`lv_port.c`) não é escolha estética: o driver `esp_lcd_axs15231b`
  pula o comando RASET (0x2B) em modo QSPI, o que quebra renderização parcial
  ([esp-bsp#724](https://github.com/espressif/esp-bsp/issues/724)). Refresh completo contorna.
- **Rotação é sempre por software.** O painel ignora MADCTL, então rotação em runtime não
  funciona em nenhuma stack; `LVGL_PORT_ROTATION_DEGREE` em `DEMO_LVGL.c` resolve em tempo
  de compilação (90° = landscape 480×320).
- **O touch é capacitivo e não precisa de calibração** — o que importa é o remapeamento de
  coordenadas conforme a rotação, feito em `lv_port.c`.
- **Supervisor de conexão** (`herdr_ws.c`): o `esp_websocket_client` não se recupera de
  conexões half-open (TCP aberto que para de receber, sem FIN) — o device ficava preso em
  "conectando" indefinidamente. Como o relay faz broadcast a cada 2 s, silêncio por 30 s
  significa conexão morta, e o cliente é reiniciado.

## Créditos e licença

O BSP, os drivers de display/touch e o port da LVGL vêm do
[port PlatformIO de NorthernMan54](https://github.com/NorthernMan54/JC3248W535EN),
que empacota o material original do fabricante (Shenzhen Jingcai / Guition). A LVGL 8.4
está vendorizada em `libraries/lvgl`, podada dos demos e exemplos que não entram no build.

MIT — veja `LICENSE`.
