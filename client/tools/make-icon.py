"""iiv-client のアイコンを作る。

    python tools/make-icon.py

src/iiv-client.ico   緑のノート PC と、画面へ入ってくる矢印
src/iiv-client.png   README 用
"""
import math, os
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]

GREEN = (22, 163, 74, 255)
GREEN_DARK = (21, 128, 61, 255)
SCREEN = (20, 83, 45, 255)
LIGHT = (134, 239, 172, 255)
WHITE = (255, 255, 255, 255)


def arrow(p0, p1, shaft, head):
    """p0 から p1 へ向かう矢印の多角形(p1 が先)"""
    dx, dy = p1[0] - p0[0], p1[1] - p0[1]
    n = math.hypot(dx, dy)
    ux, uy = dx / n, dy / n
    px, py = -uy, ux
    bx, by = p1[0] - ux * head, p1[1] - uy * head
    s, h = shaft / 2, head * 0.85
    return [(p0[0] + px * s, p0[1] + py * s), (bx + px * s, by + py * s), (bx + px * h, by + py * h), p1,
            (bx - px * h, by - py * h), (bx - px * s, by - py * s), (p0[0] - px * s, p0[1] - py * s)]


def draw(size):
    s = 1024
    im = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # ふた(画面)
    d.rounded_rectangle([150, 150, 874, 700], radius=60, fill=GREEN)
    d.rounded_rectangle([215, 215, 809, 640], radius=24, fill=SCREEN)
    # 本体(キーボードの側): 手前が広い台形
    d.polygon([(130, 720), (894, 720), (1000, 860), (24, 860)], fill=GREEN_DARK)
    d.rounded_rectangle([24, 840, 1000, 900], radius=30, fill=GREEN_DARK)
    d.rounded_rectangle([420, 735, 604, 775], radius=18, fill=LIGHT)     # タッチパッド
    # 画面へ入ってくる矢印(左下へ)
    d.polygon(arrow((760, 260), (330, 590), 130, 220), fill=WHITE)
    return im.resize((size, size), Image.LANCZOS)


imgs = [draw(n) for n in SIZES]
imgs[-1].save(os.path.join(ROOT, 'src', 'iiv-client.ico'), format='ICO', sizes=[(n, n) for n in SIZES], append_images=imgs[:-1])
imgs[-1].resize((128, 128), Image.LANCZOS).save(os.path.join(ROOT, 'src', 'iiv-client.png'))
print('ok')
