#!/usr/bin/env python3
"""Gera um catálogo animado das animações do Sonic para escolher a olho.

Frame parado engana: várias animações do jogo só se distinguem em movimento
(a $0C parece reclamar num frame estático, mas animada é o Sonic gritando
enquanto cai). Este script monta um GIF por animação, na velocidade declarada
no próprio script de animação do jogo, mais um index.html para folhear tudo.

    python3 scripts/sonic_preview.py && open scripts/sonic_anims/index.html

A saída é descartável e fica fora do git. Para usar uma animação escolhida
aqui, leve o id dela para o dict ANIMS de sonic_export.py.

Sprites © SEGA — uso pessoal, não redistribuir.
"""
import importlib.util
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("se", HERE / "sonic_export.py")
se = importlib.util.module_from_spec(spec)
spec.loader.exec_module(se)

OUT = HERE / "sonic_anims"
ZOOM = 4
BG = (24, 24, 28)
# Andar e correr trazem duração >= $F0: no jogo ela sai da velocidade do Sonic,
# não da tabela. Para o preview vale um ritmo médio.
SPEED_DRIVEN = 0xF0
DEFAULT_TICKS = 6
TICK_MS = 1000 / 60

# O disassembly não nomeia as animações, então aqui vai o que se vê no sprite —
# descrição, não nome oficial.
NOTES = {
    0x00: "andar", 0x01: "correr", 0x02: "rolar", 0x03: "rolar (igual à $02)",
    0x04: "empurrar", 0x05: "espera — parado, bate o pé, coça o queixo, aponta",
    0x06: "equilíbrio na beirada", 0x07: "cabeça baixa, mãos juntas",
    0x08: "agachar", 0x09: "spin dash", 0x0A: "agachar e ficar",
    0x0B: "escorregar", 0x0C: "braços agitando, boca aberta (caindo)",
    0x0D: "braço estendido à frente", 0x0E: "deitado (1 frame)",
    0x0F: "giro deitado", 0x10: "parado, 1 frame lento",
    0x11: "deitado deslizando", 0x12: "pendurado", 0x13: "comemorar — balança o dedo",
    0x14: "mãos para cima", 0x15: "levanta e anda", 0x16: "eletrocutado",
    0x17: "afogando", 0x18: "de frente, mãos para cima", 0x19: "caído (KO)",
    0x1A: "machucado, 1 frame lento", 0x1B: "machucado", 0x1C: "de pé, bem lento",
    0x1D: "caído", 0x1E: "giro deitado (variante)", 0x1F: "virar Super Sonic",
    0x20: "andar (variante)", 0x21: "andar (1 frame)", 0x22: "empurrar",
    0x23: "empurrar (igual à $22)",
}


def read_anim(block):
    """Duração, sequência de frames e passo de loop, como o jogo declara."""
    raw = list(block)
    dur, steps, loop = raw[0], [], 0
    i = 1
    while i < len(raw):
        b = raw[i]
        if b == 0xFF:            # volta ao começo
            break
        if b == 0xFE:            # volta N passos
            loop = max(0, len(steps) - raw[i + 1])
            break
        if b == 0xFD:            # troca de animação: para o preview, encerra
            break
        if b >= 0xFC:
            break
        steps.append(b)
        i += 1
    return dur, steps, loop


def main():
    art = (se.SONIC / "Art" / "Sonic.bin").read_bytes()
    pal = se.load_palette(se.SONIC / "Palettes" / "SonicAndTails.bin")
    map_order, map_blocks = se.parse_asm(se.SONIC / "Map - Sonic.asm")
    dplc_order, dplc_blocks = se.parse_asm(se.SONIC / "DPLC - Sonic.asm")
    anim_order, anim_blocks = se.parse_asm(se.SONIC / "Anim - Sonic.asm")

    OUT.mkdir(exist_ok=True)
    for old in OUT.glob("*"):
        old.unlink()

    cards = []
    for aid in range(0x24):
        label = "AniSonic%02X" % aid
        if label not in anim_blocks:
            continue
        dur, steps, loop = read_anim(anim_blocks[label])
        if not steps:
            continue

        rendered = {}
        for f in set(steps):
            try:
                rendered[f] = se.render_frame(
                    bytes(map_blocks[map_order[f]]),
                    se.build_vram(bytes(dplc_blocks[dplc_order[f]]), art, pal))
            except (ValueError, IndexError, KeyError):
                pass
        steps = [f for f in steps if f in rendered]
        if not steps:
            continue

        # canvas comum alinhado pelo hotspot do jogo, como faz o exportador
        minx = min(o[0] for _, o in rendered.values())
        miny = min(o[1] for _, o in rendered.values())
        w = max(o[0] + im.width for im, o in rendered.values()) - minx
        h = max(o[1] + im.height for im, o in rendered.values()) - miny

        pics = []
        for f in steps:
            im, (x0, y0) = rendered[f]
            canvas = Image.new("RGB", (w, h), BG)
            canvas.paste(im, (x0 - minx, y0 - miny), im)
            pics.append(canvas.resize((w * ZOOM, h * ZOOM), Image.NEAREST))

        ticks = DEFAULT_TICKS if dur >= SPEED_DRIVEN else dur + 1
        ms = max(20, round(ticks * TICK_MS))
        name = "%02X.gif" % aid
        pics[0].save(OUT / name, save_all=True, append_images=pics[1:],
                     duration=ms, loop=0)
        cards.append((aid, name, len(steps), len(set(steps)), ms, loop, dur,
                      NOTES.get(aid, "")))
        print("$%02X  %2d passos  %3d ms/frame  %s" % (aid, len(steps), ms, NOTES.get(aid, "")))

    rows = "\n".join(
        f'<figure><img src="{n}" alt="AniSonic{a:02X}">'
        f'<figcaption><b>${a:02X}</b> {note}<br>'
        f'<small>{steps} passos · {uniq} frames · {ms} ms/frame'
        f'{" · loop no passo %d" % loop if loop else ""}'
        f'{" · velocidade vem da corrida" if dur >= SPEED_DRIVEN else ""}</small>'
        f'</figcaption></figure>'
        for a, n, steps, uniq, ms, loop, dur, note in cards)

    (OUT / "index.html").write_text(f"""<!doctype html>
<meta charset="utf-8"><title>Animações do Sonic</title>
<style>
 body{{background:#0d0d0f;color:#ececec;font:14px -apple-system,sans-serif;margin:24px}}
 h1{{font-size:18px;font-weight:600}} p{{color:#7c7c82;max-width:60ch}}
 .grid{{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:16px;margin-top:20px}}
 figure{{margin:0;background:#17171a;border:1px solid #26262a;border-radius:8px;padding:12px;text-align:center}}
 img{{image-rendering:pixelated;max-width:100%}}
 figcaption{{margin-top:8px;font-size:13px;line-height:1.5}}
 small{{color:#7c7c82;font-size:11px}}
</style>
<h1>Animações do Sonic 3 &amp; Knuckles</h1>
<p>As {len(cards)} animações do personagem, na velocidade declarada no jogo.
A descrição é o que se vê no sprite — o disassembly não as nomeia.
Escolha um <b>$id</b> e leve para o dict <code>ANIMS</code> de
<code>sonic_export.py</code>.</p>
<div class="grid">
{rows}
</div>
""", encoding="utf-8")
    print(f"\n{len(cards)} animações em {OUT}/index.html")


if __name__ == "__main__":
    main()
