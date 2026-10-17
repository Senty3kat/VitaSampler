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

# дорожки платы: ортогональные линии с изломом 45 градусов, площадки на концах
tr = Image.new("RGBA", (W, H), (0, 0, 0, 0))
d = ImageDraw.Draw(tr)
TRACE = (46, 120, 134, 46)
PAD = (46, 120, 134, 70)
regions = [(14, 362 + 8, 292, 530), (300, 362 + 6, 830, 366 + 4), (14, 44, 20, 350)]
def route(x, y, n):
    pts = [(x, y)]
    for _ in range(n):
        dx, dy = rng.choice([(1, 0), (-1, 0), (0, 1), (0, -1)])
        L = rng.randint(16, 60)
        x2, y2 = x + dx * L, y + dy * L
        pts.append((x2, y2)); x, y = x2, y2
        if rng.random() < 0.5:
            sx, sy = rng.choice([-1, 1]), rng.choice([-1, 1])
            x2, y2 = x + sx * 10, y + sy * 10
            pts.append((x2, y2)); x, y = x2, y2
    return pts
# дорожки в полосах корпуса вокруг панелей и по краям (под экраном и кнопками их почти не видно)
areas = [(0, 0, W, 40), (0, 354, W, 8), (0, 536, W, 8), (0, 44, 10, 312), (952, 44, 8, 312), (884, 44, 12, 312)]
for _ in range(34):
    ax, ay, bx, by = rng.choice(areas)
    x, y = rng.randint(ax, max(ax, bx - 1)), rng.randint(ay, max(ay, by - 1))
    pts = route(x, y, rng.randint(3, 7))
    d.line(pts, fill=TRACE, width=1)
    for (px, py) in (pts[0], pts[-1]):
        d.ellipse((px - 2, py - 2, px + 2, py + 2), fill=PAD)
# микросхема-призрак в левом верхнем углу панели управления и два дросселя справа: тонкий контур
d.rectangle((300, 372, 316, 380), outline=(46, 120, 134, 40))
tr_a = np.asarray(tr, np.float32) / 255.0
a = tr_a[..., 3:4]
img[..., :3] = img[..., :3] * (1 - a) + tr_a[..., :3] * a

# диагональный блик пластика (верхний левый свет): широкая мягкая полоса
yy, xx = np.mgrid[0:H, 0:W]
u = (xx * 0.6 + yy) / (W * 0.6 + H)
sheen = np.exp(-((u - 0.22) / 0.07) ** 2) * 0.10 + np.exp(-((u - 0.62) / 0.03) ** 2) * 0.05
img[..., :3] = img[..., :3] * (1 - sheen[..., None]) + sheen[..., None]
img[..., :3] = img[..., :3].clip(0, 1)
Image.fromarray((img * 255 + 0.5).astype(np.uint8), "RGBA").save(os.path.join(HERE, "..", "assets", "t1_body.png"))
print("t1_body.png")
