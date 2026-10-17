#!/usr/bin/env python3
"""Спрайты кнопок и лампочек темы Aqua в духе Winamp-скинов: assets/t1_keys.png (208x104: 4 цвета x вверх/вниз по 52x52, 9-slice с краем 22)
и assets/t1_lamps.png (240x40: красная/зелёная/голубая, выкл/вкл по 40x40, центр 20,20). Запуск: python3 tools/gen_theme_aqua_sprites.py"""
import numpy as np, os
from PIL import Image

SS = 4
OUT = os.path.join(os.path.dirname(__file__), "..", "assets")


def grid(w, h):
    ys, xs = np.mgrid[0:h * SS, 0:w * SS]
    return (xs + 0.5) / SS, (ys + 0.5) / SS


def new(w, h):
    return np.zeros((h * SS, w * SS, 4), np.float32)


def over(dst, rgb, a):
    a = np.clip(a, 0, 1)[..., None]
    da = dst[..., 3:4]
    oa = a + da * (1 - a)
    dst[..., :3] = (np.asarray(rgb, np.float32) * a + dst[..., :3] * da * (1 - a)) / np.maximum(oa, 1e-6)
    dst[..., 3:4] = oa


def cover(d):
    return np.clip(0.5 - d * SS, 0, 1)


def rrect_sd(x, y, x0, y0, w, h, r):
    cx, cy = x0 + w / 2, y0 + h / 2
    qx = np.abs(x - cx) - (w / 2 - r)
    qy = np.abs(y - cy) - (h / 2 - r)
    return np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r


def down(img):
    h, w = img.shape[0] // SS, img.shape[1] // SS
    pm = img.copy()
    pm[..., :3] *= pm[..., 3:4]
    pm = pm.reshape(h, SS, w, SS, 4).mean((1, 3))
    a = pm[..., 3:4]
    rgb = pm[..., :3] / np.maximum(a, 1e-6)
    return Image.fromarray(np.clip(np.concatenate([rgb, a * 255], -1) + 0.5, 0, 255).astype(np.uint8), "RGBA")


def vgrad(y, y0, y1, c0, c1):
    t = np.clip((y - y0) / max(y1 - y0, 1e-6), 0, 1)[..., None]
    return np.asarray(c0, np.float32) * (1 - t) + np.asarray(c1, np.float32) * t


# цвета кнопок: (верх лица, низ лица, стенка, кромка)
KEYS = {
    "ice":   ((236, 251, 252), (112, 188, 200), (58, 128, 144), (22, 92, 112)),
    "cyan":  ((176, 250, 250), (28, 190, 204),  (14, 112, 132), (8, 78, 100)),
    "green": ((190, 250, 200), (54, 190, 96),   (24, 112, 62),  (12, 78, 44)),
    "steel": ((214, 226, 232), (118, 140, 154), (66, 84, 98),   (30, 52, 68)),
}


def key(name, pressed):
    top, bot, wall, edge = KEYS[name]
    W = H = 52
    x, y = grid(W, H)
    im = new(W, H)
    bx, by, bw, bh = 8, 5, 36, 36
    off = 2.5 if pressed else 0.0
    wall_h = 1.5 if pressed else 4.0
    r = 17.5
    if not pressed:                                   # стенка (передняя грань) под лицом
        d = rrect_sd(x, y, bx, by + wall_h, bw, bh, r)
        over(im, wall, cover(d))
        over(im, edge, cover(np.abs(d + 0.5) - 0.5))
    d = rrect_sd(x, y, bx, by + off, bw, bh, r)
    f0, f1 = (bot, top) if pressed else (top, bot)
    face = vgrad(y, by + off, by + off + bh, f0, f1)
    over(im, face, cover(d))
    # глянцевая «пиксельная» полоса: верхняя половина светлее с жёсткой границей
    gl = (y < by + off + bh * 0.46) & (d < -2.0)
    if not pressed:
        over(im, (255, 255, 255), cover(d + 2.0) * gl * 0.34)
    over(im, edge, cover(np.abs(d + 0.6) - 0.6))      # кромка
    if not pressed:                                   # светлая подсветка сверху-слева (1 пиксель внутри кромки)
        hi = (d > -2.2) & (d < -1.1) & ((y - (by + off)) < (x - bx) * 0.9 + 6)
        over(im, (255, 255, 255), hi * 0.85)
    else:                                             # нажатая: тёмный «вдавленный» верхний край
        sh = (d > -2.6) & (d < -1.2) & (y < by + off + 6)
        over(im, edge, sh * 0.35)
    return down(im)


def lamp(color, on):
    """шарик в круглой оправе: серебристая оправа с тёмной обводкой, тёмная лунка, камень"""
    W = H = 40
    x, y = grid(W, H)
    im = new(W, H)
    cx = cy = 20.0
    rr = np.hypot(x - cx, y - cy)
    if on:                                            # свечение
        for k, a in ((19, 0.06), (16, 0.10), (13.5, 0.16)):
            over(im, color, cover(rr - k) * a)
    c0 = np.asarray(color, np.float32)
    over(im, (6, 40, 58), cover(rr - 9.6))            # тёмная обводка
    if on:
        gem = vgrad(y, cy - 8.5, cy + 8.5, np.minimum(c0 * 1.3 + 50, 255), c0 * 0.75)
    else:
        gem = vgrad(y, cy - 8.5, cy + 8.5, c0 * 0.38 + 14, c0 * 0.14 + 6)
    over(im, gem, cover(rr - 8.2))
    hl = rrect_sd(x, y, cx - 5.2, cy - 6.6, 10.4, 5.2, 2.6)   # овальный блик сверху
    over(im, (255, 255, 255), cover(hl) * (0.9 if on else 0.35))
    return down(im)


def main():
    keys = Image.new("RGBA", (208, 104))
    for i, n in enumerate(("ice", "cyan", "green", "steel")):
        keys.paste(key(n, False), (i * 52, 0))
        keys.paste(key(n, True), (i * 52, 52))
    keys.save(os.path.join(OUT, "t1_keys.png"))
    lamps = Image.new("RGBA", (240, 40))
    cols = [(255, 62, 56), (70, 232, 110), (70, 190, 255)]
    for i, c in enumerate(cols):
        lamps.paste(lamp(c, False), ((i * 2) * 40, 0))
        lamps.paste(lamp(c, True), ((i * 2 + 1) * 40, 0))
    lamps.save(os.path.join(OUT, "t1_lamps.png"))
    print("t1_keys.png t1_lamps.png")


main()
