#!/usr/bin/env python3
"""Empacota os sprites RLE de um avatar num arquivo .hav (só stdlib).

Lê os `src/assets/sprite_*.h` gerados pelos scripts de export e escreve o
formato que o motor do painel carrega do cartão SD (ver src/avatar_pack.h).
Os bytes de RLE saem idênticos aos que hoje ficam em flash — o que o pacote
acrescenta é a tabela de animações, que antes vivia hardcoded em cada driver.

    python3 scripts/hav_pack.py                 # todos, em packs/
    python3 scripts/hav_pack.py clawd sonic     # só os pedidos

O clawd também é copiado para src/assets/clawd.hav, que o firmware embute.
"""
from __future__ import annotations

import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "src", "assets")
OUT = os.path.join(ROOT, "packs")

MAGIC = b"HAV1"
VERSION = 1

# Papéis: 0..4 são os mesmos valores de avatar_state_t, na mesma ordem — o
# motor indexa direto pelo estado, sem tabela de tradução.
ROLES = {"disconnected": 0, "idle": 1, "done": 2, "working": 3, "blocked": 4,
         "sleep": 5, "transition": 6}

# Modo de escala. Os três vieram dos drivers que este formato substitui:
#   fit      escala até caber, ampliando o quanto precisar (clawd)
#   integer  idem, arredondado para múltiplo inteiro — pixel art nítida
#   shrink   nunca amplia além do nativo, mas reduz para caber (cenas largas)
ZOOM = {"fit": 0, "integer": 1, "shrink": 2}

FLAG_NO_ANTIALIAS = 1 << 2

# Uma entrada por avatar. `anims` é (papel, sufixo do sprite, ms por frame);
# a ordem aqui é a ordem no arquivo e não tem significado — quem manda é o papel.
PACKS = {
    "clawd": dict(
        name="Clawd", sym="", key=0x18C5, zoom="fit", antialias=True, sleep_s=180,
        anims=[("disconnected", "dizzy", 125), ("idle", "idle", 167),
               ("sleep", "sleeping", 167), ("working", "typing", 125),
               ("done", "happy", 100), ("blocked", "alert", 100)]),
    "sonic": dict(
        name="Sonic", sym="sonic_", key=0x18C5, zoom="integer", antialias=False,
        sleep_s=180, seqs="sonic_sequences.h",
        anims=[("disconnected", "ko", 600), ("idle", "idle", 100),
               ("sleep", "sleep", 167), ("working", "run", 60),
               ("done", "cheer", 133), ("blocked", "push", 200)]),
    "mcqueen": dict(
        name="McQueen", sym="mcqueen_", key=0x18C5, zoom="integer", antialias=False,
        sleep_s=180,
        anims=[("disconnected", "disconnected", 160), ("idle", "idle", 90),
               ("sleep", "sleep", 150), ("working", "working", 60),
               ("done", "done", 70), ("blocked", "blocked", 100)]),
    "spiderman": dict(
        name="Spider-Man", sym="spidey_", key=0x18C5, zoom="integer", antialias=False,
        sleep_s=0,
        anims=[("idle", "idle", 130), ("working", "crawl", 90),
               ("done", "cheer", 110), ("blocked", "web", 120),
               ("disconnected", "down", 150), ("transition", "flip", 90)]),
    "sf": dict(
        name="Ryu vs Ken", sym="sf_", key=0x18C5, zoom="shrink", antialias=True,
        sleep_s=0,
        anims=[("working", "luta", 68), ("idle", "encarando", 120),
               ("blocked", "guarda", 130), ("done", "vitoria", 150),
               ("disconnected", "caidos", 180)]),
}


def _array(text: str, decl: str):
    """Valores de `static const <tipo> <decl>[...] = { ... };` como ints."""
    m = re.search(re.escape(decl) + r"\s*\[[^\]]*\]\s*=\s*\{(.*?)\}\s*;", text, re.S)
    if not m:
        raise SystemExit(f"não achei o array {decl}")
    return [int(v, 0) for v in re.findall(r"0[xX][0-9a-fA-F]+|\d+", m.group(1))]


def _define(text: str, name: str) -> int:
    m = re.search(r"#define\s+" + re.escape(name) + r"\s+(\S+)", text)
    if not m:
        raise SystemExit(f"não achei o #define {name}")
    return int(m.group(1), 0)


def build(pack_id: str) -> bytes:
    spec = PACKS[pack_id]
    sym = spec["sym"]
    seq_text = ""
    if spec.get("seqs"):
        seq_text = open(os.path.join(ASSETS, spec["seqs"]), encoding="utf-8").read()

    table, blocks = [], []
    # os blocos começam depois do cabeçalho e da tabela, ambos de tamanho fixo
    off = 32 + 20 * len(spec["anims"])

    for role, suffix, frame_ms in spec["anims"]:
        text = open(os.path.join(ASSETS, f"sprite_{sym}{suffix}.h"), encoding="utf-8").read()
        up = f"{sym}{suffix}".upper()
        w, h = _define(text, f"{up}_WIDTH"), _define(text, f"{up}_HEIGHT")
        frames = _define(text, f"{up}_FRAME_COUNT")
        offsets = _array(text, f"{sym}{suffix}_frame_offsets")
        rle = _array(text, f"{sym}{suffix}_rle_data")

        if len(offsets) != frames + 1:
            raise SystemExit(f"{pack_id}/{suffix}: {len(offsets)} offsets para {frames} frames")
        if offsets[frames] != len(rle):
            raise SystemExit(f"{pack_id}/{suffix}: sentinela {offsets[frames]} != {len(rle)} words")

        seq, seq_loop = [], 0
        if seq_text and f"{sym}{suffix}_seq" in seq_text:
            seq = _array(seq_text, f"{sym}{suffix}_seq")
            seq_loop = _define(seq_text, f"{up}_SEQ_LOOP")
            if max(seq) >= frames:
                raise SystemExit(f"{pack_id}/{suffix}: seq aponta para frame inexistente")

        # frame_offsets (4B) primeiro, rle (2B) depois, seq (1B) por último:
        # nessa ordem cada array cai alinhado sem byte de enchimento no meio
        blob = (struct.pack(f"<{len(offsets)}I", *offsets)
                + struct.pack(f"<{len(rle)}H", *rle)
                + bytes(seq))
        blob += b"\0" * (-len(blob) % 4)

        # one-shot só faz sentido na transição: os demais papéis são estados que
        # duram enquanto o estado durar
        loop = 0 if role == "transition" else 1
        table.append(struct.pack("<BBHHHHHHHI", ROLES[role], loop, frame_ms,
                                 w, h, frames, len(seq), seq_loop, 0, off))
        blocks.append(blob)
        off += len(blob)

    flags = ZOOM[spec["zoom"]] | (0 if spec["antialias"] else FLAG_NO_ANTIALIAS)
    header = struct.pack("<4sBBHBBH20s", MAGIC, VERSION, flags, spec["key"],
                         len(spec["anims"]), 0, spec["sleep_s"],
                         spec["name"].encode("utf-8")[:19])
    return header + b"".join(table) + b"".join(blocks)


def main(argv):
    ids = argv[1:] or list(PACKS)
    os.makedirs(OUT, exist_ok=True)
    for pack_id in ids:
        if pack_id not in PACKS:
            raise SystemExit(f"avatar desconhecido: {pack_id}")
        data = build(pack_id)
        path = os.path.join(OUT, f"{pack_id}.hav")
        with open(path, "wb") as fh:
            fh.write(data)
        print(f"{len(data):>9,}  {os.path.relpath(path, ROOT)}")
        if pack_id == "clawd":
            # o de fábrica também vai para src/assets, embutido pelo EMBED_FILES
            with open(os.path.join(ASSETS, "clawd.hav"), "wb") as fh:
                fh.write(data)
            print(f"{len(data):>9,}  src/assets/clawd.hav (embutido no firmware)")


if __name__ == "__main__":
    main(sys.argv)
