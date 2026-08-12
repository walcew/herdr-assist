#!/usr/bin/env python3
"""Renderiza sprites do Lightning McQueen a partir do modelo OBJ do jogo de NDS.

O modelo e estatico (OBJ nao guarda esqueleto nem keyframes), entao as animacoes
sao sintetizadas por movimento de camera/corpo: giro, balanco, aceleracao etc.

Rasterizador proprio (z-buffer + textura por coordenada baricentrica) para nao
depender de Blender. Sao 590 triangulos, roda em Python puro sem problema.
"""

import colorsys
import math
import os
from PIL import Image, ImageSequence

RAIZ = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "Lightning Mcqueen")
OBJ = os.path.join(RAIZ, "mcqueen.obj")
MTL = os.path.join(RAIZ, "mcqueen.mtl")

# Luz direcional em espaco de camera e piso de luz ambiente.
LUZ = (-0.35, 0.55, 0.75)
AMBIENTE = 0.42


def carrega_obj(caminho):
    """Le o OBJ e devolve (vertices, uvs, normais, faces).

    Cada face e ((vi, ti, ni) x3, material), com indices ja em base zero.
    """
    vertices, uvs, normais, faces = [], [], [], []
    material = None
    with open(caminho) as arq:
        for linha in arq:
            campos = linha.split()
            if not campos:
                continue
            tipo = campos[0]
            if tipo == "v":
                vertices.append(tuple(map(float, campos[1:4])))
            elif tipo == "vt":
                uvs.append(tuple(map(float, campos[1:3])))
            elif tipo == "vn":
                normais.append(tuple(map(float, campos[1:4])))
            elif tipo == "usemtl":
                material = campos[1]
            elif tipo == "f":
                canto = []
                for parte in campos[1:4]:
                    pedacos = (parte.split("/") + ["", ""])[:3]
                    canto.append(tuple(int(p) - 1 if p else -1 for p in pedacos))
                faces.append((canto, material))
    return vertices, uvs, normais, faces


# A textura ripada veio com as cores desviadas (corpo rosa-arroxeado, raio
# verde-limao, pneus azuis) e nenhuma permutacao de canal RGB corrige. Este
# passe reaproxima a paleta da referencia oficial, classificando cada texel por
# matiz e mantendo o sombreamento (V do HSV). So se aplica a textura do corpo;
# a dos olhos (064) ja e azul de fabrica, como no filme.
RODA = (0, 31, 42, 59)  # caixa x0,x1,y0,y1 da roda na textura: azul vira pneu/cubo


def _corrige_texel(x, y, r, g, b):
    hue, sat, val = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
    hue *= 360.0

    if RODA[0] <= x <= RODA[1] and RODA[2] <= y <= RODA[3] and 170 <= hue <= 300:
        if val < 0.5:      # borracha do pneu
            hue, sat, val = 0, 0.10, val * 0.55
        else:              # aro/cubo
            hue, sat, val = 357, 0.72, val * 0.80
    elif sat < 0.15:
        pass               # cinzas ficam como estao
    elif hue >= 295 or hue < 28:   # lataria rosa-arroxeada -> vermelho vivo
        hue, sat, val = 357, min(1.0, sat * 1.9 + 0.05), min(1.0, val * 1.28 + 0.03)
    elif hue < 45:                 # laranjas so ganham saturacao
        sat = min(1.0, sat * 1.2)
    elif hue < 170:                # raio/decalques verde-limao -> amarelo-ouro
        hue, sat, val = 47, min(1.0, sat * 1.35), min(1.0, val * 1.18)
    elif val >= 0.5:               # vidros/brilhos azulados -> menos azul
        sat *= 0.45
    else:                          # azuis escuros (sombras) -> quase preto
        sat, val = sat * 0.3, val * 0.6

    r2, g2, b2 = (int(round(c * 255)) for c in colorsys.hsv_to_rgb(hue / 360.0, sat, val))
    # preto puro e reservado a cor-chave de transparencia
    return (1, 1, 1) if (r2, g2, b2) == (0, 0, 0) else (r2, g2, b2)


def _corrige_textura(img):
    px = img.load()
    for y in range(img.height):
        for x in range(img.width):
            cor = px[x, y]
            if cor != (0, 0, 0):
                px[x, y] = _corrige_texel(x, y, *cor)
    return img


def carrega_texturas(caminho_mtl, corrigir=True):
    """Devolve {material: (pixels, largura, altura)} lendo os map_Kd do MTL."""
    texturas = {}
    material = None
    with open(caminho_mtl) as arq:
        for linha in arq:
            campos = linha.split()
            if not campos:
                continue
            if campos[0] == "newmtl":
                material = campos[1]
            elif campos[0] == "map_Kd" and material:
                # O nome do arquivo tem espaco ("NDS Texture.062.png"), entao
                # pega tudo depois da chave em vez de confiar no split.
                nome = linha.split("map_Kd", 1)[1].strip()
                img = Image.open(os.path.join(RAIZ, nome)).convert("RGB")
                if corrigir and material == CORPO:
                    img = _corrige_textura(img)
                texturas[material] = (img.load(), img.width, img.height)
    return texturas


# --- rosto -------------------------------------------------------------------
# A textura 064 (64x32) e so o para-brisa com os olhos, num material proprio
# (NDS_Material.065, 8 faces). O jogo nao traz estados alternativos de olho,
# entao piscada e direcao do olhar sao geradas mexendo nessa textura: a palpebra
# roxa que ja existe no topo desce sobre a esclera, e as iris deslizam na
# horizontal. Coordenadas medidas na textura original.
OLHOS = "NDS_Material.065"
PALPEBRA_Y = 8          # linha de onde sai a cor da palpebra (com seu gradiente)
ESCLERA_Y0, ESCLERA_Y1 = 10, 23
ROSTO_X0, ROSTO_X1 = 12, 52   # trecho da esclera onde as iris podem correr
IRIS_Y0, IRIS_Y1 = 11, 20
LIMIAR_IRIS = 400             # abaixo disso o pixel e iris/pupila, nao esclera


def textura_rosto(base, fechamento=0.0, desvio=0):
    """Devolve a textura dos olhos com a palpebra baixada e/ou as iris deslocadas.

    fechamento 0.0 = olhos abertos, 1.0 = totalmente fechados
    desvio     deslocamento horizontal das iris, em pixels de textura
    """
    pixels, largura, altura = base
    grade = [[pixels[x, y] for x in range(largura)] for y in range(altura)]

    if desvio:
        # Detecta a iris por cor, e nao por caixa fixa: o halo azul em volta da
        # pupila faz parte dela e, se ficar para tras, vira uma iris fantasma.
        for y in range(IRIS_Y0, IRIS_Y1 + 1):
            linha = grade[y]
            faixa = range(ROSTO_X0, ROSTO_X1 + 1)
            iris = {x: linha[x] for x in faixa if sum(linha[x]) < LIMIAR_IRIS}
            claros = [linha[x] for x in faixa if sum(linha[x]) >= LIMIAR_IRIS]
            if not iris or not claros:
                continue
            esclera = max(set(claros), key=claros.count)
            for x in iris:
                linha[x] = esclera
            for x, cor in iris.items():
                destino = x + desvio
                if ROSTO_X0 <= destino <= ROSTO_X1:
                    linha[destino] = cor

    if fechamento > 0:
        limite = ESCLERA_Y0 + fechamento * (ESCLERA_Y1 - ESCLERA_Y0)
        for y in range(ESCLERA_Y0, min(altura, int(limite) + 1)):
            for x in range(largura):
                # preto puro e a cor-chave de transparencia: nao invadir a borda
                if grade[y][x] != (0, 0, 0):
                    grade[y][x] = pixels[x, PALPEBRA_Y]

    imagem = Image.new("RGB", (largura, altura))
    imagem.putdata([c for linha in grade for c in linha])
    return (imagem.load(), largura, altura)


# A boca mora na textura do corpo (062), no canto superior esquerdo: fenda
# escura em y 5-9, labio superior rosado acima, faixa clara de labio inferior em
# y 10-11. (Rastreada pelo buffer de indice de faces do render frontal - o
# circulo maior mais abaixo na textura e o adesivo do capo, nao a boca.) A caixa
# para em x 38 / y 13 para nao invadir os cantos verdes de outra peca. Como nao
# ha quadros alternativos, a abertura vem de esticar/comprimir a faixa na
# vertical.
CORPO = "NDS_Material.063"
BOCA_X0, BOCA_X1 = 17, 38
BOCA_Y0, BOCA_Y1 = 2, 13


def textura_boca(base, abertura=0.0, curva=0.0):
    """Devolve a textura do corpo com a boca deformada.

    abertura  estica (>0) ou comprime (<0) a faixa na vertical: abre/fecha
    curva     ergue os cantos por uma parabola, em px de textura: sorriso.
              Negativo derruba os cantos (boca triste).
    """
    pixels, largura, altura = base
    if not abertura and not curva:
        return base

    meio = (BOCA_Y0 + BOCA_Y1) / 2.0
    fator = max(0.05, 1.0 + abertura)
    imagem = Image.new("RGB", (largura, altura))
    imagem.putdata([pixels[x, y] for y in range(altura) for x in range(largura)])
    destino = imagem.load()

    for x in range(BOCA_X0, BOCA_X1 + 1):
        # A tira e MEIA boca, espelhada pelas faces: centro da boca em BOCA_X0 e
        # canto em BOCA_X1 (verificado erguendo cada lado e olhando o render).
        # A rampa sobe so do lado do canto (subir = conteudo vem de baixo,
        # somando a origem).
        ergue = curva * ((x - BOCA_X0) / (BOCA_X1 - BOCA_X0)) ** 2
        for y in range(BOCA_Y0, BOCA_Y1 + 1):
            origem = meio + (y - meio) / fator + ergue
            origem = min(BOCA_Y1, max(BOCA_Y0, int(round(origem))))
            destino[x, y] = pixels[x, origem]
    return (destino, largura, altura)


def _centro_e_escala(vertices):
    """Centro do bounding box e maior extensao, para enquadrar o modelo."""
    eixos = list(zip(*vertices))
    centro = tuple((min(e) + max(e)) / 2 for e in eixos)
    extensao = max(max(e) - min(e) for e in eixos)
    return centro, extensao


def renderiza(vertices, uvs, normais, faces, texturas, *, giro=0.0, inclinacao=18.0,
              rolagem=0.0, altura=0.0, zoom=1.0, tamanho=160, supersample=3,
              culling=-1, luz=True):
    """Rasteriza um frame e devolve um PIL.Image RGBA com fundo transparente.

    giro       graus em torno de Y (eixo vertical) - turntable
    inclinacao graus de camera olhando de cima
    rolagem    graus em torno de Z (inclina o carro de lado)
    altura     deslocamento vertical em fracao do tamanho do sprite
    zoom       fator de aproximacao
    """
    lado = tamanho * supersample
    centro, extensao = _centro_e_escala(vertices)
    escala = lado / extensao * 0.92 * zoom

    # Inclinacao positiva olha o carro de cima; o eixo X do modelo cresce para
    # baixo na tela, por isso o sinal invertido aqui.
    ry, rx, rz = map(math.radians, (giro, -inclinacao, rolagem))
    cos_y, sin_y = math.cos(ry), math.sin(ry)
    cos_x, sin_x = math.cos(rx), math.sin(rx)
    cos_z, sin_z = math.cos(rz), math.sin(rz)

    def transforma(ponto, centraliza=True):
        x, y, z = ponto
        if centraliza:
            x, y, z = x - centro[0], y - centro[1], z - centro[2]
        # Giro em Y, depois inclinacao em X, depois rolagem em Z.
        x, z = x * cos_y + z * sin_y, -x * sin_y + z * cos_y
        y, z = y * cos_x - z * sin_x, y * sin_x + z * cos_x
        x, y = x * cos_z - y * sin_z, x * sin_z + y * cos_z
        return x, y, z

    meio = lado / 2.0
    desloc_y = altura * lado
    cor = [0] * (lado * lado * 4)
    profundidade = [1e30] * (lado * lado)

    for canto, material in faces:
        textura = texturas.get(material)
        if textura is None:
            continue
        pixels, tex_w, tex_h = textura

        tela, coord_uv, luzes = [], [], []
        for vi, ti, ni in canto:
            x, y, z = transforma(vertices[vi])
            tela.append((meio + x * escala, meio - y * escala - desloc_y, z))
            coord_uv.append(uvs[ti] if 0 <= ti < len(uvs) else (0.0, 0.0))
            if not luz:
                luzes.append(1.0)
                continue
            nx, ny, nz = transforma(normais[ni], centraliza=False) if 0 <= ni < len(normais) else (0, 0, 1)
            norma = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
            # Faces de casca unica podem ter normal apontando para dentro; usa o
            # valor absoluto para iluminar os dois lados igualmente.
            difusa = abs(nx * LUZ[0] + ny * LUZ[1] + nz * LUZ[2]) / norma
            luzes.append(min(1.0, AMBIENTE + 0.75 * difusa))

        (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = tela
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if area == 0:
            continue
        # O rip de NDS tem faces de casca unica, entao o padrao e nao descartar
        # nada e deixar o z-buffer resolver a visibilidade (culling=0).
        if (culling > 0 and area > 0) or (culling < 0 and area < 0):
            continue

        min_x = max(0, int(min(x0, x1, x2)))
        max_x = min(lado - 1, int(max(x0, x1, x2)) + 1)
        min_y = max(0, int(min(y0, y1, y2)))
        max_y = min(lado - 1, int(max(y0, y1, y2)) + 1)
        if min_x > max_x or min_y > max_y:
            continue

        for py in range(min_y, max_y + 1):
            cy = py + 0.5
            base = py * lado
            for px in range(min_x, max_x + 1):
                cx = px + 0.5
                # Coordenadas baricentricas; projecao e ortografica, logo a
                # interpolacao linear de UV e exata (sem correcao de perspectiva).
                w0 = ((x1 - cx) * (y2 - cy) - (x2 - cx) * (y1 - cy)) / area
                if w0 < 0:
                    continue
                w1 = ((x2 - cx) * (y0 - cy) - (x0 - cx) * (y2 - cy)) / area
                if w1 < 0:
                    continue
                w2 = 1.0 - w0 - w1
                if w2 < 0:
                    continue

                z = w0 * z0 + w1 * z1 + w2 * z2
                indice = base + px
                if z >= profundidade[indice]:
                    continue

                u = w0 * coord_uv[0][0] + w1 * coord_uv[1][0] + w2 * coord_uv[2][0]
                v = w0 * coord_uv[0][1] + w1 * coord_uv[1][1] + w2 * coord_uv[2][1]
                tx = int(u * tex_w) % tex_w
                ty = int((1.0 - v) * tex_h) % tex_h
                r, g, b = pixels[tx, ty]
                # O NDS usa preto puro como cor-chave de transparencia; sem isso
                # as faces vazadas (grade do motor, vaos) viram blocos opacos.
                if r == 0 and g == 0 and b == 0:
                    continue

                # So agora o pixel e opaco de fato e pode ocupar o z-buffer.
                profundidade[indice] = z
                brilho = w0 * luzes[0] + w1 * luzes[1] + w2 * luzes[2]
                destino = indice * 4
                cor[destino] = min(255, int(r * brilho))
                cor[destino + 1] = min(255, int(g * brilho))
                cor[destino + 2] = min(255, int(b * brilho))
                cor[destino + 3] = 255

    quadro = Image.frombytes("RGBA", (lado, lado), bytes(cor))
    if supersample > 1:
        quadro = quadro.resize((tamanho, tamanho), Image.LANCZOS)
    return quadro


# Aberturas de boca para a fala. Uma senoide pura fica robotica: com periodo
# inteiro ela devolve poucos valores distintos e todos simetricos. Fala de
# verdade tem silabas de tamanhos diferentes, entao a sequencia e irregular de
# proposito - 17 passos, primo com os 18 e 24 frames que a usam.
VISEMAS = (0.0, 0.75, 1.15, 0.40, -0.20, 0.90, 1.25, 0.55,
           0.10, -0.30, 0.65, 1.05, 0.30, -0.15, 0.80, 1.20, 0.45)

# No modelo, giro=180 e a frente (rosto). 210 da um 3/4 frontal que mostra ao
# mesmo tempo os olhos e o 95 da lateral - o angulo mais legivel para o avatar.
FRENTE = 180.0
GIRO_PADRAO = 210.0

# Camera padronizada das animacoes de status: mesmo angulo em todas.
ST_CAM = dict(giro=GIRO_PADRAO, inclinacao=14.0, zoom=1.35)

# Cada animacao define o intervalo entre frames e uma funcao que devolve os
# parametros de render do frame i (de n). Como o OBJ nao tem esqueleto, o
# movimento vem todo de camera e corpo.
ANIMACOES = [
    ("giro", "giro completo de 360 graus, comecando de frente", 24, 80,
     lambda i, n: dict(giro=FRENTE + 360.0 * i / n)),

    ("giro_lento", "giro completo, meia velocidade", 36, 140,
     lambda i, n: dict(giro=FRENTE + 360.0 * i / n)),

    ("parado", "parado, respirando (sobe e desce de leve)", 16, 90,
     lambda i, n: dict(giro=GIRO_PADRAO,
                       altura=0.012 * math.sin(2 * math.pi * i / n))),

    ("olhando", "balanca o corpo para os lados, procurando", 20, 90,
     lambda i, n: dict(giro=GIRO_PADRAO + 22.0 * math.sin(2 * math.pi * i / n))),

    ("acelerar", "empina a frente e assenta, arrancada", 12, 70,
     lambda i, n: dict(giro=GIRO_PADRAO,
                       inclinacao=18.0 - 16.0 * math.sin(math.pi * i / n))),

    ("frear", "mergulha a frente, freada brusca", 10, 70,
     lambda i, n: dict(giro=GIRO_PADRAO,
                       inclinacao=18.0 + 14.0 * math.sin(math.pi * i / n))),

    ("curva", "inclina para um lado e para o outro", 20, 90,
     lambda i, n: dict(giro=GIRO_PADRAO,
                       rolagem=9.0 * math.sin(2 * math.pi * i / n))),

    ("pulo", "salta e cai", 12, 60,
     lambda i, n: dict(giro=GIRO_PADRAO,
                       altura=0.16 * math.sin(math.pi * i / n))),

    ("tremer", "vibrando no lugar, motor ligado", 8, 45,
     lambda i, n: dict(giro=GIRO_PADRAO + (1.6 if i % 2 else -1.6),
                       altura=0.008 * (1 if i % 2 else -1))),

    ("comemorar", "gira e pula ao mesmo tempo", 24, 70,
     lambda i, n: dict(giro=FRENTE + 360.0 * i / n,
                       altura=0.10 * abs(math.sin(2 * math.pi * i / n)),
                       rolagem=7.0 * math.sin(4 * math.pi * i / n))),

    # --- rosto: de frente e com zoom, senao os olhos somem no sprite ---
    ("piscar", "de frente, pisca os olhos", 16, 90,
     lambda i, n: dict(giro=FRENTE, inclinacao=10.0, zoom=1.7,
                       fechamento=_piscada(i, n))),

    ("olhar", "de frente, olha de um lado ao outro", 20, 100,
     lambda i, n: dict(giro=FRENTE, inclinacao=10.0, zoom=1.7,
                       desvio=round(6 * math.sin(2 * math.pi * i / n)))),

    ("sono", "olhos pesados, cabeceando", 16, 130,
     lambda i, n: dict(giro=FRENTE, inclinacao=10.0, zoom=1.7,
                       fechamento=0.58 + 0.34 * math.sin(2 * math.pi * i / n),
                       altura=-0.012 * math.sin(2 * math.pi * i / n))),

    ("acordar", "abre os olhos devagar", 12, 100,
     lambda i, n: dict(giro=FRENTE, inclinacao=10.0, zoom=1.7,
                       fechamento=max(0.0, 1.0 - 1.15 * i / (n - 1)))),

    ("atencao", "encara, pisca e volta a encarar", 24, 80,
     lambda i, n: dict(giro=FRENTE, inclinacao=10.0, zoom=1.7,
                       fechamento=_piscada(i, n),
                       desvio=round(4 * math.sin(4 * math.pi * i / n)))),

    # --- boca ---
    ("falar", "abre e fecha a boca, falando", 18, 80,
     lambda i, n: dict(giro=FRENTE, inclinacao=10.0, zoom=1.7,
                       abertura=VISEMAS[i % len(VISEMAS)])),

    ("sorrir", "cantos da boca sobem: sorriso largo", 16, 100,
     lambda i, n: dict(giro=FRENTE, inclinacao=10.0, zoom=1.7,
                       curva=4.5 * math.sin(math.pi * i / n),
                       abertura=0.3 * math.sin(math.pi * i / n))),

    ("bocejar", "boca abre devagar e os olhos fecham junto", 20, 120,
     lambda i, n: dict(giro=FRENTE, inclinacao=10.0, zoom=1.7,
                       abertura=1.0 * math.sin(math.pi * i / n),
                       fechamento=0.9 * math.sin(math.pi * i / n))),

    ("conversar", "fala e pisca ao mesmo tempo", 24, 85,
     lambda i, n: dict(giro=FRENTE, inclinacao=10.0, zoom=1.7,
                       abertura=VISEMAS[i % len(VISEMAS)],
                       fechamento=_piscada(i, n))),

    # --- status do avatar do herdr-assist (avatar.h: avatar_state_t) ---------
    # Uma animacao por estado do motor, combinando corpo + olhos + boca. O
    # driver do Sonic mapeia DISCONNECTED->ko, WORKING->run, DONE->cheer,
    # BLOCKED->push, IDLE->idle (+sleep apos timeout); aqui e o equivalente
    # McQueen, com o rosto fazendo o papel que os membros do Sonic fazem la.
    # Camera unica (ST_CAM) em todos: 3/4 frontal, meio de cima, zoom maximo
    # que nao corta nos extremos (pulo do st_done, rolagem do st_blocked).

    # Sorriso timido no meio do ciclo: neutro ate 35%, os cantos sobem de leve
    # (metade da curva do sorriso largo), seguram e desfazem ate 75% - depois a
    # piscada fecha o ciclo. Le como quem sorri sozinho de vez em quando.
    ("st_idle", "AVATAR_ST_IDLE — em espera: sorri de leve e desfaz, o olhar passeia, pisca", 24, 90,
     lambda i, n: dict(ST_CAM,
                       altura=0.010 * math.sin(2 * math.pi * i / n),
                       desvio=round(2.2 * math.sin(2 * math.pi * i / n)),
                       curva=2.2 * math.sin(math.pi * min(1.0, max(0.0, (i / n - 0.35) / 0.4))),
                       fechamento=_piscada(i, n))),

    # Tremor leve (so um fio de vibracao) + o olhar de atencao da anim "atencao":
    # olhos varrendo enquanto trabalha, piscada no fim do ciclo.
    ("st_working", "AVATAR_ST_WORKING — trabalhando: vibra de leve, olhar atento, arranca a cada ciclo", 16, 60,
     lambda i, n: dict(ST_CAM,
                       giro=GIRO_PADRAO + (0.7 if i % 2 else -0.7),
                       inclinacao=14.0 - 10.0 * max(0.0, math.sin(2 * math.pi * i / n)) ** 2,
                       altura=0.003 * (1 if i % 2 else -1),
                       desvio=round(4 * math.sin(4 * math.pi * i / n)),
                       fechamento=_piscada(i, n))),

    # Corpo da anim "comemorar" (giro 360 + pulos + rolagem), partindo do angulo
    # padrao para casar com os demais estados; sorriso aberto o tempo todo, que
    # aparece quando o rosto passa pela camera. Zoom menor porque o giro passa
    # pela lateral inteira do carro (1.35 cortaria).
    ("st_done", "AVATAR_ST_DONE — terminou: comemora, giro completo com pulos, sorrindo", 24, 70,
     lambda i, n: dict(ST_CAM,
                       zoom=1.05,
                       giro=GIRO_PADRAO + 360.0 * i / n,
                       altura=0.10 * abs(math.sin(2 * math.pi * i / n)),
                       rolagem=7.0 * math.sin(4 * math.pi * i / n),
                       curva=4.5, abertura=0.3)),

    # Fala (VISEMAS a 100 ms, um pouco mais calma que "falar") por cima dos
    # cantos preocupados: le como quem explica o que precisa para ser liberado.
    ("st_blocked", "AVATAR_ST_BLOCKED — aguardando aprovação: preocupado, fala procurando o usuário", 24, 100,
     lambda i, n: dict(ST_CAM,
                       curva=-2.5,
                       abertura=VISEMAS[i % len(VISEMAS)],
                       desvio=round(5 * math.sin(2 * math.pi * i / n)),
                       rolagem=2.0 * math.sin(4 * math.pi * i / n),
                       fechamento=_piscada(i, n))),

    ("st_disconnected", "AVATAR_ST_DISCONNECTED — sem host: apagado, olhos fechados, caído", 12, 160,
     lambda i, n: dict(ST_CAM,
                       fechamento=1.0, curva=-3.0,
                       altura=-0.012 + 0.005 * math.sin(2 * math.pi * i / n))),

    ("st_sleep", "IDLE após timeout — dormindo: pálpebras pesadas, cabeceia devagar", 16, 150,
     lambda i, n: dict(ST_CAM,
                       fechamento=0.8 + 0.2 * math.sin(2 * math.pi * i / n),
                       abertura=0.15,
                       altura=-0.008 * math.sin(2 * math.pi * i / n))),
]


def _piscada(i, n):
    """Olhos abertos quase o ciclo todo e uma piscada rapida no fim."""
    fase = i / n
    if fase < 0.72:
        return 0.0
    return math.sin((fase - 0.72) / 0.28 * math.pi)

# Fundo dos cards da pagina; os GIFs sao compostos sobre ele para as bordas
# suavizadas nao ficarem serrilhadas (GIF so tem transparencia de 1 bit).
FUNDO = (23, 23, 26)

# Animacoes que mexem no rosto, e nao no corpo - a pagina as separa.
ROSTO = {"piscar", "olhar", "sono", "acordar", "atencao",
         "falar", "sorrir", "bocejar", "conversar"}


def gera_animacoes(destino, tamanho=160):
    """Renderiza cada animacao como um GIF em `destino` e devolve os metadados."""
    cena = carrega_obj(OBJ)
    texturas = carrega_texturas(MTL)
    os.makedirs(destino, exist_ok=True)

    catalogo = []
    for nome, descricao, frames, ms, parametros in ANIMACOES:
        imagens = []
        for i in range(frames):
            padrao = dict(giro=GIRO_PADRAO, inclinacao=18.0, tamanho=tamanho)
            padrao.update(parametros(i, frames))

            # Estes tres nao vao para o rasterizador: reescrevem as texturas do
            # rosto antes do render - olhos na 064, boca na 062.
            fechamento = padrao.pop("fechamento", 0.0)
            desvio = padrao.pop("desvio", 0)
            abertura = padrao.pop("abertura", 0.0)
            curva = padrao.pop("curva", 0.0)
            do_frame = texturas
            if fechamento or desvio or abertura or curva:
                do_frame = dict(texturas)
                if fechamento or desvio:
                    do_frame[OLHOS] = textura_rosto(texturas[OLHOS], fechamento, desvio)
                if abertura or curva:
                    do_frame[CORPO] = textura_boca(texturas[CORPO], abertura, curva)

            quadro = renderiza(*cena, do_frame, **padrao)
            fundo = Image.new("RGBA", quadro.size, FUNDO + (255,))
            fundo.alpha_composite(quadro)
            imagens.append(fundo.convert("RGB").quantize(colors=255, method=Image.MEDIANCUT))

        caminho = os.path.join(destino, nome + ".gif")
        imagens[0].save(caminho, save_all=True, append_images=imagens[1:],
                        duration=ms, loop=0, optimize=True)

        # Le de volta o que foi gravado: o GIF arredonda a duracao para 10 ms e
        # funde frames identicos consecutivos, entao os numeros da pagina saem
        # do arquivo, nunca do que foi pedido.
        with Image.open(caminho) as gif:
            duracoes = [q.info.get("duration", 0) for q in ImageSequence.Iterator(gif)]
        catalogo.append(dict(nome=nome, descricao=descricao, frames=frames,
                             frames_gif=len(duracoes), ciclo=sum(duracoes),
                             kb=os.path.getsize(caminho) / 1024))
        print(f"  {nome:12} {frames:3} frames  ciclo {sum(duracoes):5} ms  "
              f"{os.path.getsize(caminho)/1024:6.1f} KB")
    return catalogo


CABECALHO = """<!doctype html>
<meta charset="utf-8"><title>Animações do Lightning McQueen</title>
<style>
 body{background:#0d0d0f;color:#ececec;font:14px -apple-system,sans-serif;margin:24px}
 h1{font-size:18px;font-weight:600}
 h2{font-size:14px;font-weight:600;margin:28px 0 0;color:#b9b9bf}
 p{color:#7c7c82;max-width:64ch}
 .aviso{border-left:2px solid #6b4a1f;background:#1a1610;padding:10px 14px;margin:16px 0;max-width:64ch}
 .aviso b{color:#d2a24c}
 .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:16px;margin-top:12px}
 figure{margin:0;background:#17171a;border:1px solid #26262a;border-radius:8px;padding:12px;text-align:center}
 img{image-rendering:pixelated;max-width:100%;border-radius:4px}
 figcaption{margin-top:8px;font-size:13px;line-height:1.5}
 small{color:#7c7c82;font-size:11px}
 code{background:#1e1e22;padding:1px 5px;border-radius:3px;font-size:12px}
</style>
<h1>Animações do Lightning McQueen</h1>
<p>Renderizadas a partir de <code>mcqueen.obj</code> — o rip do modelo 3D do jogo
de NDS. Escolha um <b>nome</b> e leve para a lista <code>ANIMACOES</code> de
<code>mcqueen_export.py</code> para ajustar frames, velocidade ou tamanho.
Esta página é gerada por esse mesmo script: os números saem dos arquivos.</p>

<div class="aviso">
<b>O modelo não tem animação própria.</b> OBJ guarda só a malha — sem esqueleto e
sem keyframes, e as texturas trazem um único rosto: olhos abertos, boca parada.
Não há quadros alternativos escondidos (as áreas que a malha não usa nos dois
atlas são só espaço vazio do packing). Todo o movimento abaixo é sintetizado.<br><br>
<b>Corpo</b> por câmera: giro, balanço, inclinação e rolagem.<br>
<b>Olhos</b> reescrevendo a textura 064 a cada frame — a pálpebra roxa que já
existe desce sobre a esclera, e as íris deslizam na horizontal.<br>
<b>Boca</b> deformando a faixa da boca no para-choque dentro da textura do corpo
(x 17–38, y 2–13): esticar/comprimir na vertical abre e fecha; erguer os cantos
por uma rampa curva o traço em sorriso. A tira é meia boca, espelhada pelas
faces — centro em x 17, canto em x 38.<br><br>
<b>As cores passam por correção.</b> A textura ripada veio desviada (corpo
rosa-arroxeado, raio verde-limão, pneus azuis) e nenhuma permutação de canal RGB
resolve. Um passe por matiz (<code>_corrige_texel</code>) reaproxima a paleta da
referência oficial — vermelho vivo na lataria, amarelo-ouro nos decalques, pneu
escuro com cubo vermelho — preservando o sombreamento. A textura dos olhos já é
azul de fábrica e não é alterada.
</div>

<p>A seção <b>Status do avatar</b> traz uma animação por estado de
<code>avatar.h</code> (<code>avatar_state_t</code>), composta a partir das
peças de rosto e corpo — candidatas ao driver <code>avatar_mcqueen</code> do
firmware, no papel que ko/run/cheer/push/idle/sleep fazem no driver do Sonic.</p>
"""


def gera_pagina(catalogo, destino):
    """Escreve o index.html a partir dos metadados lidos dos GIFs."""
    grupos = (
        ("Status do avatar (herdr-assist)", lambda a: a["nome"].startswith("st_")),
        ("Rosto", lambda a: a["nome"] in ROSTO),
        ("Corpo", lambda a: not a["nome"].startswith("st_") and a["nome"] not in ROSTO),
    )
    partes = [CABECALHO]
    for titulo, filtro in grupos:
        itens = [a for a in catalogo if filtro(a)]
        partes.append(f'\n<h2>{titulo}</h2>\n<div class="grid">')
        for a in itens:
            fundidos = ("" if a["frames_gif"] == a["frames"]
                        else f" · {a['frames_gif']} no arquivo (iguais fundidos)")
            partes.append(
                f'<figure><img src="{a["nome"]}.gif" alt="{a["nome"]}">'
                f'<figcaption><b>{a["nome"]}</b> {a["descricao"]}<br>'
                f'<small>{a["frames"]} frames · ciclo {a["ciclo"]/1000:.2f}s · '
                f'{a["kb"]:.0f} KB{fundidos}</small></figcaption></figure>')
        partes.append("</div>")

    caminho = os.path.join(destino, "index.html")
    with open(caminho, "w") as arq:
        arq.write("\n".join(partes) + "\n")
    return caminho


# --- export para o firmware --------------------------------------------------
# Mesmo contrato dos sprites do Sonic (rle_sprite.h): pares (RGB565, contagem)
# em uint16, offsets em words com sentinela, chave transparente 0x18C5. Como o
# render 3D suavizado comprime mal em RLE, os frames do firmware saem menores
# (128 px, ~ o tamanho de tela do Sonic no slot), sem supersample e quantizados
# numa paleta unica por animacao (sem dither): regioes chapadas -> runs longos
# e nada de cintilacao de paleta entre frames.
TAM_FW = 128
CORES_FW = 48
CHAVE = 0x18C5

FRAMES_FW = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mcqueen_frames")
ASSETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "assets")


def _para_565(r, g, b):
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    if v == CHAVE:
        v = CHAVE - 1   # colisao com a chave de transparencia
    return v


def _rle_frame(img):
    """RGBA -> lista de pares (valor565, contagem), varrendo linha a linha."""
    px = img.load()
    valores = []
    for y in range(img.height):
        for x in range(img.width):
            r, g, b, a = px[x, y]
            valores.append(CHAVE if a < 128 else _para_565(r, g, b))
    pares = []
    i = 0
    while i < len(valores):
        j = i
        while j < len(valores) and valores[j] == valores[i] and j - i < 65535:
            j += 1
        pares.append((valores[i], j - i))
        i = j
    return pares


def _quantiza(frames):
    """Paleta unica por animacao: amostra 4 frames, aplica a todos sem dither."""
    amostra = Image.new("RGB", (frames[0].width * 4, frames[0].height), (0, 0, 0))
    for k, idx in enumerate(sorted({0, len(frames) // 3, len(frames) // 2,
                                    2 * len(frames) // 3})):
        chapa = Image.new("RGB", frames[idx].size, (0, 0, 0))
        chapa.paste(frames[idx], (0, 0), frames[idx])
        amostra.paste(chapa, (k * frames[0].width, 0))
    paleta = amostra.quantize(colors=CORES_FW, method=Image.MEDIANCUT)

    saida = []
    for quadro in frames:
        chapa = Image.new("RGB", quadro.size, (0, 0, 0))
        chapa.paste(quadro, (0, 0), quadro)
        plana = chapa.quantize(palette=paleta, dither=0).convert("RGB")
        novo = Image.new("RGBA", quadro.size)
        np_, qp, pp = novo.load(), quadro.load(), plana.load()
        for y in range(quadro.height):
            for x in range(quadro.width):
                np_[x, y] = pp[x, y] + (255,) if qp[x, y][3] >= 128 else (0, 0, 0, 0)
        saida.append(novo)
    return saida


def _escreve_header(caminho, nome, frames_rle, largura, altura):
    lo, up = "mcqueen_" + nome, "MCQUEEN_" + nome.upper()
    offsets, total = [0], 0
    for pares in frames_rle:
        total += len(pares) * 2
        offsets.append(total)
    bruto = largura * altura * 2 * len(frames_rle)

    linhas = [
        f"#ifndef {up}_FRAMES_H",
        f"#define {up}_FRAMES_H",
        "",
        "/**",
        " * Auto-gerado por scripts/mcqueen_export.py (RLE, contrato rle_sprite.h)",
        f" * {len(frames_rle)} frame(s), {largura}x{altura} pixels",
        f" * Raw: {bruto:,} bytes, RLE: {total * 2:,} bytes",
        f" * Transparent key: 0x{CHAVE:04X}",
        " */",
        "",
        "#include <stdint.h>",
        "",
        f"#define {up}_WIDTH  {largura}",
        f"#define {up}_HEIGHT {altura}",
        f"#define {up}_FRAME_COUNT {len(frames_rle)}",
        "",
        f"static const uint32_t {lo}_frame_offsets[{len(offsets)}] = {{",
    ]
    for i in range(0, len(offsets), 8):
        linhas.append("    " + " ".join(f"{o}," for o in offsets[i:i + 8]))
    linhas += ["};", "", f"static const uint16_t {lo}_rle_data[] = {{"]
    plano = [x for pares in frames_rle for par in pares for x in par]
    for i in range(0, len(plano), 8):
        linhas.append("    " + " ".join(
            (f"0x{v:04X}," if k % 2 == 0 else f"{v},")
            for k, v in enumerate(plano[i:i + 8], start=i)))
    linhas += ["};", "", f"#endif // {up}_FRAMES_H", ""]

    with open(caminho, "w") as arq:
        arq.write("\n".join(linhas))
    return total * 2


def gera_firmware():
    """Exporta as animacoes st_* para src/assets/sprite_mcqueen_<nome>.h."""
    cena = carrega_obj(OBJ)
    texturas = carrega_texturas(MTL)
    os.makedirs(FRAMES_FW, exist_ok=True)
    total = 0

    print(f"export firmware ({TAM_FW}px, {CORES_FW} cores/anim):")
    for nome, _, frames, ms, parametros in ANIMACOES:
        if not nome.startswith("st_"):
            continue
        curto = nome[3:]
        quadros = []
        for i in range(frames):
            padrao = dict(giro=GIRO_PADRAO, inclinacao=18.0,
                          tamanho=TAM_FW, supersample=1)
            padrao.update(parametros(i, frames))
            fechamento = padrao.pop("fechamento", 0.0)
            desvio = padrao.pop("desvio", 0)
            abertura = padrao.pop("abertura", 0.0)
            curva = padrao.pop("curva", 0.0)
            do_frame = texturas
            if fechamento or desvio or abertura or curva:
                do_frame = dict(texturas)
                if fechamento or desvio:
                    do_frame[OLHOS] = textura_rosto(texturas[OLHOS], fechamento, desvio)
                if abertura or curva:
                    do_frame[CORPO] = textura_boca(texturas[CORPO], abertura, curva)
            quadros.append(renderiza(*cena, do_frame, **padrao))

        quadros = _quantiza(quadros)
        for i, quadro in enumerate(quadros):
            quadro.save(os.path.join(FRAMES_FW, f"{curto}_{i:02}.png"))

        frames_rle = [_rle_frame(q) for q in quadros]
        caminho = os.path.join(ASSETS, f"sprite_mcqueen_{curto}.h")
        tamanho = _escreve_header(caminho, curto, frames_rle, TAM_FW, TAM_FW)
        total += tamanho
        print(f"  {curto:13} {frames:3} frames  {ms:3} ms  {tamanho / 1024:7.1f} KB")
    print(f"total RLE: {total / 1024 / 1024:.2f} MB em {ASSETS}")


if __name__ == "__main__":
    import sys

    if "--fw" in sys.argv:
        gera_firmware()
    else:
        destino = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mcqueen_anims")
        print("renderizando animacoes do McQueen:")
        catalogo = gera_animacoes(destino)
        pagina = gera_pagina(catalogo, destino)
        print(f"\n{len(catalogo)} animacoes em {destino}")
        print("pagina:", pagina)
