#!/usr/bin/env python3
"""Renderiza os mockups de tela do manual de pareamento (docs/images/).

Fiéis ao código da UI: paleta e medidas de src/ui_theme.{h,c}, textos de
src/i18n.h, fontes reais de scripts/.fontcache (o gen_font_ui.sh baixa) e da
JetBrainsMono Nerd do gen_font.sh. O QR é gerado com o MESMO qrcodegen
vendorizado que o firmware usa (compilado com cc na hora), replicando a
escolha de versão do lv_qrcode — o QR da imagem é o QR que a tela mostra.

Uso, a partir da raiz do repo:  python3 scripts/gen_docs_mockups.py
Requer Pillow e cc (Xcode CLT). Rode de novo quando a tela de pareamento
ou os textos mudarem.
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "scripts", ".fontcache")
OUT = os.path.join(ROOT, "docs", "images")
QRGEN = os.path.join(ROOT, "libraries", "lvgl", "src", "extra", "libs", "qrcode")

S = 2                      # superamostragem: o manual mostra nítido em retina
W, H = 320, 480            # painel em portrait

# --- paleta (ui_theme.h) ---
BG, PANEL, BORDER = "#0d0d0f", "#17171a", "#26262a"
TEXT, MUTED = "#ececec", "#7c7c82"
WORKING, IDLE, BLOCKED = "#c9a24a", "#7da97d", "#c05a55"
TERM_BG, TERM_TEXT = "#0a0a0b", "#aebfa0"

# --- ícones FontAwesome usados (mesmos codepoints do firmware) ---
FA_WIFI, FA_PLUS, FA_RIGHT, FA_SAVE, FA_LEFT = "", "", "", "", ""
FA_HOME, FA_LIST, FA_DASH, FA_COG = "", "", "", ""

CMD_INSTALL = "herdr plugin install walcew/herdr-assist/plugin"
CMD_ADMIN = "herdr plugin pane open --plugin herdr-assist --entrypoint admin"

# textos por idioma, espelhando src/i18n.h (pareamento) e a tela principal
T = {
    "en": {
        "settings": "Settings", "wifi_sec": "Wi-Fi network",
        "ssid": "HomeWiFi", "wifi_st": "Connected · -52 dBm (good)",
        "change": "Change ", "hosts_sec": "herdr hosts",
        "pair_btn": "  Pair with a host", "add_btn": "  Add manually",
        "device_sec": "Device", "lang": "Language", "lang_v": "English",
        "lock": "Screen lock", "lock_v": "Off", "restart": "Restart device",
        "fw": "Firmware", "fw_v": "v0.7.0", "fw_up": "Update firmware",
        "pair_title": "Pair", "this_panel": "This panel",
        "pick": "Pick this code on the host",
        "waiting": "Waiting for a host... (172s)",
        "p1": "On the host (the machine running Herdr, on the same "
              "Wi-Fi network as the panel):\n\n"
              "1. Install the bridge plugin — only once:",
        "p2": "2. In a Herdr pane, open the admin screen:",
        "p3": "3. Press p (Pair panel) and pick the code shown above "
              "from the list.\n\n"
              "The host sends name, address and token — nothing is "
              "typed on the panel. Windows or SSH without a TUI: "
              "python3 pair.py (see the manual).",
        "manual": "Full manual — point your phone camera here:",
        "url": "https://github.com/walcew/herdr-assist/blob/main/docs/pairing.md",
        "suffix": "",
    },
    "pt": {
        "settings": "Configurações", "wifi_sec": "Rede Wi-Fi",
        "ssid": "MinhaRede", "wifi_st": "Conectado · -52 dBm (bom)",
        "change": "Trocar ", "hosts_sec": "Hosts herdr",
        "pair_btn": "  Parear com um host", "add_btn": "  Adicionar manualmente",
        "device_sec": "Dispositivo", "lang": "Idioma", "lang_v": "Português",
        "lock": "Bloqueio de tela", "lock_v": "Desativado",
        "restart": "Reiniciar dispositivo",
        "fw": "Firmware", "fw_v": "v0.7.0", "fw_up": "Atualizar firmware",
        "pair_title": "Parear", "this_panel": "Este painel",
        "pick": "Escolha este código no host",
        "waiting": "Aguardando um host... (172s)",
        "p1": "No host (a máquina com o Herdr, na mesma rede Wi-Fi "
              "do painel):\n\n"
              "1. Instale o plugin da ponte — uma vez só:",
        "p2": "2. Num pane do Herdr, abra a tela de administração:",
        "p3": "3. Tecle p (Parear painel) e escolha na lista o código "
              "mostrado acima.\n\n"
              "O host envia nome, endereço e token — nada é digitado "
              "no painel. Windows ou SSH sem TUI: python3 pair.py "
              "(veja o manual).",
        "manual": "Manual completo — aponte a câmera do celular:",
        "url": "https://github.com/walcew/herdr-assist/blob/main/docs/pairing.pt-BR.md",
        "suffix": ".pt-BR",
    },
}

DEVICE_ID = "4B5E94"


# --- fontes -------------------------------------------------------------------

def _load(path, px):
    return ImageFont.truetype(path, px * S)


def load_fonts():
    mont = os.path.join(CACHE, "Montserrat-Medium.ttf")
    bold = os.path.join(CACHE, "Montserrat-Bold.ttf")
    fa = os.path.join(CACHE, "FontAwesome5.woff")
    if not os.path.exists(mont):
        sys.exit("faltam fontes em scripts/.fontcache — rode scripts/gen_font_ui.sh antes")
    try:
        fa_probe = ImageFont.truetype(fa, 16)
    except OSError:
        # FreeType sem WOFF: descomprime com fontTools se houver
        from fontTools.ttLib import TTFont
        ttf = os.path.join(CACHE, "FontAwesome5.ttf")
        if not os.path.exists(ttf):
            TTFont(fa).save(ttf)
        fa = ttf
        fa_probe = ImageFont.truetype(fa, 16)
    del fa_probe
    mono = os.path.expanduser("~/Library/Fonts/JetBrainsMonoNerdFont-Regular.ttf")
    if not os.path.exists(mono):
        mono = "/System/Library/Fonts/Menlo.ttc"
    return {
        "ui12": _load(mont, 12), "ui14": _load(mont, 14), "ui16": _load(mont, 16),
        "bold20": _load(bold, 20), "clock44": _load(mont, 44),
        "fa14": _load(fa, 14), "fa16": _load(fa, 16), "fa20": _load(fa, 20),
        "fa12": _load(fa, 12), "mono12": _load(mono, 12),
    }


# --- QR: mesmo caminho do lv_qrcode -------------------------------------------

QR_DUMP_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qrcodegen.h"
/* stub do logger da LVGL, que o qrcodegen referencia com LV_USE_LOG 1 */
void _lv_log_add(int level, const char *file, int line,
                 const char *func, const char *format, ...) {}
/* Espelha lv_qrcode_update(): minFit em ECC M + "version extend" para
   aproveitar o canvas. argv: url, lado do widget em px. */
int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    const char *url = argv[1];
    int target = atoi(argv[2]);
    size_t len = strlen(url);
    int ver = qrcodegen_getMinFitVersion(qrcodegen_Ecc_MEDIUM, len);
    if (ver <= 0) return 1;
    int qr_size = qrcodegen_version2size(ver);
    int scale = target / qr_size;
    if (scale <= 0) return 1;
    int remain = target % qr_size;
    int ext = remain / (scale << 2);
    if (ext && ver < qrcodegen_VERSION_MAX) {
        ver = ver + ext > qrcodegen_VERSION_MAX ? qrcodegen_VERSION_MAX : ver + ext;
    }
    static uint8_t qr0[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    memcpy(tmp, url, len);
    if (!qrcodegen_encodeBinary(tmp, len, qr0, qrcodegen_Ecc_MEDIUM,
                                ver, ver, qrcodegen_Mask_AUTO, true))
        return 1;
    int size = qrcodegen_getSize(qr0);
    printf("%d\n", size);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++)
            putchar(qrcodegen_getModule(qr0, x, y) ? '1' : '0');
        putchar('\n');
    }
    return 0;
}
"""


def qr_matrix(url, target_px):
    """Devolve (lado, [linhas de '0'/'1']) compilando o qrcodegen vendorizado."""
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "qr_dump.c")
        exe = os.path.join(td, "qr_dump")
        with open(src, "w", encoding="utf-8") as fh:
            fh.write(QR_DUMP_C)
        subprocess.run(["cc", "-std=c99", "-I", QRGEN,
                        "-I", os.path.join(ROOT, "src"),   # lv_conf.h do projeto
                        "-DLV_CONF_INCLUDE_SIMPLE", src,
                        os.path.join(QRGEN, "qrcodegen.c"), "-o", exe], check=True)
        out = subprocess.run([exe, url, str(target_px)], check=True,
                             capture_output=True, text=True).stdout.splitlines()
    return int(out[0]), out[1:]


# --- primitivas ---------------------------------------------------------------

def rr(d, x, y, w, h, r, **kw):
    d.rounded_rectangle([x * S, y * S, (x + w) * S, (y + h) * S], radius=r * S, **kw)


def text(d, x, y, s, font, fill, anchor="la"):
    d.text((x * S, y * S), s, font=font, fill=fill, anchor=anchor)


def wrap(d, s, font, maxw):
    """Quebra por palavra na largura (px de tela), respeitando \n."""
    lines = []
    for par in s.split("\n"):
        if not par:
            lines.append("")
            continue
        cur = ""
        for word in par.split(" "):
            cand = word if not cur else cur + " " + word
            if d.textlength(cand, font=font) <= maxw * S:
                cur = cand
                continue
            if cur:
                lines.append(cur)
            # palavra maior que a linha (URL): quebra por caractere, como a LVGL
            cur = ""
            for ch in word:
                if d.textlength(cur + ch, font=font) <= maxw * S:
                    cur += ch
                else:
                    lines.append(cur)
                    cur = ch
        lines.append(cur)
    return lines


def para(d, x, y, s, font, fill, maxw, lh):
    for ln in wrap(d, s, font, maxw):
        text(d, x, y, ln, font, fill)
        y += lh
    return y


def topbar(d, f, title, back):
    x = 12
    if back:
        rr(d, 12, 10, 44, 44, 22, fill=PANEL, outline=BORDER, width=S)
        text(d, 12 + 22, 10 + 22, FA_LEFT, f["fa20"], TEXT, anchor="mm")
        x = 12 + 44 + 10
    text(d, x, 32, title, f["bold20"], TEXT, anchor="lm")


# --- tela: Configurações (caminho até "Parear com um host") -------------------

def render_settings(lang):
    t, f = T[lang], FONTS
    img = Image.new("RGB", (W * S, H * S), BG)
    d = ImageDraw.Draw(img)

    topbar(d, f, t["settings"], back=False)
    # botão de salvar da topbar principal (verde, ícone escuro)
    rr(d, W - 12 - 44, 10, 44, 44, 22, fill=IDLE, outline=IDLE, width=S)
    text(d, W - 12 - 22, 10 + 22, FA_SAVE, f["fa20"], TERM_BG, anchor="mm")

    x, y = 8, 64
    text(d, x, y, t["wifi_sec"], f["ui12"], MUTED); y += 15 + 6

    rr(d, x, y, 304, 48, 6, fill=PANEL)
    text(d, x + 12, y + 24 - 9, t["ssid"], f["ui14"], TEXT, anchor="lm")
    text(d, x + 12, y + 24 + 10, t["wifi_st"], f["ui12"], IDLE, anchor="lm")
    cw = d.textlength(t["change"], font=f["ui12"]) / S
    text(d, x + 304 - 12 - cw - 8, y + 24, t["change"], f["ui12"], MUTED, anchor="lm")
    text(d, x + 304 - 12 - 8, y + 24, FA_RIGHT, f["fa12"], MUTED, anchor="lm")
    y += 48 + 6

    text(d, x, y, t["hosts_sec"], f["ui12"], MUTED); y += 15 + 6

    # a linha verde é o assunto do manual: contorno âmbar para o leitor achar
    rr(d, x - 3, y - 3, 310, 50, 9, outline=WORKING, width=2 * S)
    rr(d, x, y, 304, 44, 6, fill=IDLE)
    iw = d.textlength(FA_WIFI, font=f["fa14"]) / S
    tw = d.textlength(t["pair_btn"], font=f["ui14"]) / S
    bx = x + (304 - iw - tw) / 2
    text(d, bx, y + 22, FA_WIFI, f["fa14"], TERM_BG, anchor="lm")
    text(d, bx + iw, y + 22, t["pair_btn"], f["ui14"], TERM_BG, anchor="lm")
    y += 44 + 6

    rr(d, x, y, 304, 44, 6, fill=PANEL)
    iw = d.textlength(FA_PLUS, font=f["fa14"]) / S
    tw = d.textlength(t["add_btn"], font=f["ui14"]) / S
    bx = x + (304 - iw - tw) / 2
    text(d, bx, y + 22, FA_PLUS, f["fa14"], MUTED, anchor="lm")
    text(d, bx + iw, y + 22, t["add_btn"], f["ui14"], MUTED, anchor="lm")
    y += 44 + 6

    text(d, x, y, t["device_sec"], f["ui12"], MUTED); y += 15 + 6
    for left, right in ((t["lang"], t["lang_v"]), (t["lock"], t["lock_v"])):
        rr(d, x, y, 304, 44, 6, fill=PANEL)
        text(d, x + 12, y + 22, left, f["ui14"], TEXT, anchor="lm")
        text(d, x + 304 - 12, y + 22, right, f["ui14"], MUTED, anchor="rm")
        y += 44 + 6
    rr(d, x, y, 304, 44, 6, fill=PANEL)
    text(d, x + 152, y + 22, t["restart"], f["ui14"], TEXT, anchor="mm")
    y += 44 + 6
    rr(d, x, y, 304, 44, 6, fill=PANEL)
    text(d, x + 12, y + 22, t["fw"], f["ui14"], TEXT, anchor="lm")
    text(d, x + 304 - 12, y + 22, t["fw_v"], f["ui14"], MUTED, anchor="rm")
    y += 44 + 6

    # dock flutuante (Configurações ativa)
    dw, dh = 4 * 62 + 3 * 4 + 10 + 2, 50
    dx, dy = (W - dw) / 2, H - 10 - dh
    rr(d, dx, dy, dw, dh, 22, fill=PANEL, outline=BORDER, width=S)
    for i, icon in enumerate((FA_HOME, FA_LIST, FA_DASH, FA_COG)):
        ix = dx + 1 + 5 + i * (62 + 4)
        on = i == 3
        if on:
            rr(d, ix, dy + 1 + 5, 62, 38, 17, fill=TEXT)
        text(d, ix + 31, dy + 1 + 5 + 19, icon, f["fa16"],
             BG if on else MUTED, anchor="mm")

    img.save(os.path.join(OUT, f"pair-settings{t['suffix']}.png"))


# --- tela: Parear (visão completa, com rolagem esticada) ----------------------

def render_pair(lang):
    t, f = T[lang], FONTS
    img = Image.new("RGB", (W * S, 1200 * S), BG)
    d = ImageDraw.Draw(img)

    topbar(d, f, t["pair_title"], back=True)

    x, y = 8, 64
    rr(d, x, y, 304, 150, 8, fill=PANEL)
    text(d, x + 152, y + 12, t["this_panel"], f["ui12"], MUTED, anchor="ma")
    text(d, x + 152, y + 12 + 22, DEVICE_ID, f["clock44"], TEXT, anchor="ma")
    text(d, x + 152, y + 150 - 12, t["pick"], f["ui12"], MUTED, anchor="ms")
    y += 150 + 6

    y = para(d, x, y, t["waiting"], f["ui14"], WORKING, 304, 18) + 6
    y = para(d, x, y, t["p1"], f["ui12"], MUTED, 304, 15) + 6

    for cmd in (CMD_INSTALL, CMD_ADMIN):
        lines = wrap(d, cmd, f["mono12"], 304 - 16)
        ch = 8 + len(lines) * 16 + 8
        rr(d, x, y, 304, ch, 6, fill=TERM_BG)
        cy = y + 8
        for ln in lines:
            text(d, x + 8, cy, ln, f["mono12"], TERM_TEXT)
            cy += 16
        y += ch + 6
        if cmd == CMD_INSTALL:
            y = para(d, x, y, t["p2"], f["ui12"], MUTED, 304, 15) + 6

    y = para(d, x, y, t["p3"], f["ui12"], MUTED, 304, 15) + 6

    # card do manual: legenda + QR em moldura branca + URL
    size, rows = qr_matrix(t["url"], 180)
    cap = wrap(d, t["manual"], f["ui12"], 304 - 24)
    url = wrap(d, t["url"], f["ui12"], 304 - 24)
    ch = 12 + len(cap) * 15 + 10 + 208 + 10 + len(url) * 15 + 12
    rr(d, x, y, 304, ch, 8, fill=PANEL)
    cy = y + 12
    for ln in cap:
        text(d, x + 152, cy, ln, f["ui12"], MUTED, anchor="ma")
        cy += 15
    cy += 10
    qx, qy = x + (304 - 208) / 2, cy
    rr(d, qx, qy, 208, 208, 8, fill="#ffffff")
    scale = 180 // size
    margin = (208 - size * scale) / 2
    for ry, row in enumerate(rows):
        for rx, c in enumerate(row):
            if c == "1":
                px, py = qx + margin + rx * scale, qy + margin + ry * scale
                d.rectangle([px * S, py * S, (px + scale) * S - 1,
                             (py + scale) * S - 1], fill="#000000")
    cy += 208 + 10
    for ln in url:
        text(d, x + 152, cy, ln, f["ui12"], MUTED, anchor="ma")
        cy += 15
    y += ch + 16

    img = img.crop((0, 0, W * S, int(y) * S))
    img.save(os.path.join(OUT, f"pair-screen{t['suffix']}.png"))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    FONTS = load_fonts()
    for lang in ("en", "pt"):
        render_settings(lang)
        render_pair(lang)
    print("gerado: docs/images/pair-{settings,screen}[.pt-BR].png")
