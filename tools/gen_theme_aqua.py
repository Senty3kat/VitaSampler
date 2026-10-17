#!/usr/bin/env python3
"""Фон темы Aqua (assets/t1_body.png): общий body.png перекрашивается так же, как th_map в main.c, затем поверх - «прозрачная электроника»:
еле заметные дорожки платы, контактные площадки, диагональный блик пластика. Запуск: python3 tools/gen_theme_aqua.py"""
import numpy as np, os, random
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(__file__)
src = np.asarray(Image.open(os.path.join(HERE, "..", "assets", "body.png")).convert("RGBA"), np.float32) / 255.0


def th_map(a):
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    mx = a[..., :3].max(-1); mn = a[..., :3].min(-1); d = mx - mn
    s = np.where(mx > 0, d / np.maximum(mx, 1e-6), 0)
    dd = np.maximum(d, 1e-6)
    h = np.where(mx == r, (g - b) / dd + np.where(g < b, 6, 0), np.where(mx == g, (b - r) / dd + 2, (r - g) / dd + 4)) * 60
    sel = (d >= 0.02) & (h >= 14) & (h <= 75)
    k = (h - 14) / 61
    h2 = (h + 156 - 8 * k) % 360
    s2 = np.where(s < 0.30, s * 1.7 + 0.03, s).clip(0, 1)
    v = np.where(mx > 0.55, mx + (1 - mx) * 0.10, mx)
    hh = h2 / 60; f = hh - np.floor(hh); p = v * (1 - s2); q = v * (1 - s2 * f); t = v * (1 - s2 * (1 - f))
    i = np.floor(hh).astype(int) % 6
    R = np.choose(i, [v, q, p, p, t, v]); G = np.choose(i, [t, v, v, q, p, p]); B = np.choose(i, [p, p, t, v, v, q])
    out = a.copy()
    out[..., 0] = np.where(sel, R, r); out[..., 1] = np.where(sel, G, g); out[..., 2] = np.where(sel, B, b)
    return out


img = th_map(src)
W, H = 960, 544
rng = random.Random(7)

# «электроника под прозрачным корпусом»: плата с дорожками, микросхемами, конденсаторами, винтами -
# рисуется отдельным слоем, слегка размывается (матовый пластик) и просвечивает сквозь тонированный корпус
SC = 2
big = Image.new("RGBA", (W * SC, H * SC), (0, 0, 0, 0))
d = ImageDraw.Draw(big)
def S(v): return int(v * SC)
BOARD = (18, 84, 100, 255); TR = (96, 214, 214, 255); TRD = (10, 52, 66, 255); GOLD = (214, 230, 170, 255); CHIP = (14, 40, 56, 255)
d.rectangle((0, 0, W * SC, H * SC), fill=BOARD)
# сетка дорожек: пучки параллельных линий с изломами под 45 градусов
def bus(x, y, n, step, length, horiz=True):
    for i in range(n):
        o = i * step
        pts = [(x, y + o)] if horiz else [(x + o, y)]
        cx, cy = pts[0]
        for k in range(rng.randint(2, 4)):
            L = rng.randint(30, length)
            if horiz: cx += L; pts.append((cx, cy)); cx += 8 * (1 if rng.random() < .5 else -1); cy += 8 * rng.choice([-1, 1]); pts.append((cx, cy))
            else: cy += L; pts.append((cx, cy)); cy += 8; cx += 8 * rng.choice([-1, 1]); pts.append((cx, cy))
        d.line([(S(px), S(py)) for px, py in pts], fill=TRD, width=S(3.6))
        d.line([(S(px), S(py)) for px, py in pts], fill=TR, width=S(2))
        for (px, py) in (pts[0], pts[-1]):
            d.ellipse((S(px - 3), S(py - 3), S(px + 3), S(py + 3)), fill=GOLD, outline=TRD, width=S(1))
            d.ellipse((S(px - 1), S(py - 1), S(px + 1), S(py + 1)), fill=TRD)
for _ in range(26):
    bus(rng.randint(0, W - 100), rng.randint(0, H - 60), rng.randint(3, 6), 6, 140, rng.random() < .5)
# микросхемы с ножками
for _ in range(9):
    cx, cy, cw, ch = rng.randint(20, W - 120), rng.randint(10, H - 70), rng.choice([40, 56, 72]), rng.choice([24, 32])
    for i in range(0, cw, 6):
        d.rectangle((S(cx + i), S(cy - 5), S(cx + i + 3), S(cy)), fill=GOLD)
        d.rectangle((S(cx + i), S(cy + ch), S(cx + i + 3), S(cy + ch + 5)), fill=GOLD)
    d.rectangle((S(cx), S(cy), S(cx + cw), S(cy + ch)), fill=CHIP, outline=TRD, width=S(1))
    d.ellipse((S(cx + 3), S(cy + 3), S(cx + 8), S(cy + 8)), fill=(70, 150, 160, 255))
    d.line((S(cx + 12), S(cy + ch / 2), S(cx + cw - 6), S(cy + ch / 2)), fill=(40, 100, 116, 255), width=S(2))
# конденсаторы (круги) и резисторы (капсулы)
for _ in range(22):
    cx, cy = rng.randint(10, W - 10), rng.randint(10, H - 10)
    r = rng.choice([5, 7, 9])
    d.ellipse((S(cx - r), S(cy - r), S(cx + r), S(cy + r)), fill=(40, 150, 170, 255), outline=TRD, width=S(1.5))
    d.ellipse((S(cx - r + 3), S(cy - r + 3), S(cx + r - 3), S(cy + r - 3)), fill=(150, 226, 232, 255))
for _ in range(26):
    cx, cy = rng.randint(10, W - 30), rng.randint(10, H - 10)
    d.rounded_rectangle((S(cx), S(cy), S(cx + 18), S(cy + 6)), radius=S(3), fill=(210, 190, 120, 255), outline=TRD, width=S(1))
    d.rectangle((S(cx + 5), S(cy), S(cx + 7), S(cy + 6)), fill=(200, 60, 50, 255))
board = big.resize((W, H), Image.LANCZOS).filter(ImageFilter.GaussianBlur(1.1))
bd = np.asarray(board, np.float32)[..., :3] / 255.0
# корпус: тонированный полупрозрачный пластик, электроника видна на ~42%
K = np.full((H, W, 1), 0.58, np.float32)
K[0:42] = 0.30                     # верхняя планка (под вкладками и лампочками)
K[44:532, 14:946] = 0.27          # панель управления: подписи должны читаться
Kimg = Image.fromarray((K[..., 0] * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(6))
K = np.asarray(Kimg, np.float32)[..., None] / 255.0
img[..., :3] = img[..., :3] * (1 - K) + bd * K
# мягкое «молочное» посветление, чтобы пластик читался матовым
img[..., :3] = img[..., :3] * 0.90 + 0.10 * np.array([0.82, 0.94, 0.96])

# диагональный блик пластика (верхний левый свет): широкая мягкая полоса
yy, xx = np.mgrid[0:H, 0:W]
u = (xx * 0.6 + yy) / (W * 0.6 + H)
sheen = np.exp(-((u - 0.22) / 0.07) ** 2) * 0.10 + np.exp(-((u - 0.62) / 0.03) ** 2) * 0.05
img[..., :3] = img[..., :3] * (1 - sheen[..., None]) + sheen[..., None]
img[..., :3] = img[..., :3].clip(0, 1)
Image.fromarray((img * 255 + 0.5).astype(np.uint8), "RGBA").save(os.path.join(HERE, "..", "assets", "t1_body.png"))
print("t1_body.png")
