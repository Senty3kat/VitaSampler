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
    bx, by, bw, bh = 7, 5, 38, 38
    R = 18.5
    dy = 1.0 if pressed else 0.0
    # внешний серебристый ободок
    d = rrect_sd(x, y, bx, by, bw, bh + 2, R + 1)
    over(im, (10, 52, 70), cover(d))
    rim = vgrad(y, by, by + bh + 2, (250, 255, 255) if not pressed else (150, 176, 186), (140, 172, 184) if not pressed else (236, 248, 250))
    over(im, rim, cover(d + 1.4))
    # внутренняя капсула
    ix, iy, iw, ih = bx + 4, by + 4 + dy, bw - 8, bh - 8
    d2 = rrect_sd(x, y, ix, iy, iw, ih, R - 4)
    over(im, (10, 52, 70), cover(d2))
    f0, f1 = (bot, top) if pressed else (top, bot)
    face = vgrad(y, iy, iy + ih, f0, f1)
    over(im, face, cover(d2 + 1.4))
    if not pressed:
        gl = (y < iy + ih * 0.45)
        over(im, (255, 255, 255), cover(d2 + 3.0) * gl * 0.40)
    else:
        over(im, edge, cover(d2 + 1.4) * (d2 > -4.0) * (y < iy + 7) * 0.30)
    return down(im)


def lamp(color, on):
    """шарик в круглой оправе: серебристая оправа с тёмной обводкой, тёмная лунка, камень"""
    W = H = 40
    x, y = grid(W, H)
    im = new(W, H)
    cx = cy = 20.0
    rr = np.hypot(x - cx, y - cy)
    c0 = np.asarray(color, np.float32)
    over(im, (10, 52, 70), cover(rr - 10.5))          # внешняя обводка
    over(im, (222, 240, 244) if on else (170, 198, 208), cover(rr - 9.4))   # светлое кольцо
    over(im, (10, 52, 70), cover(rr - 6.8))           # обводка бусины
    if on:
        gem = vgrad(y, cy - 5.6, cy + 5.6, np.minimum(c0 * 1.3 + 60, 255), c0 * 0.85)
    else:
        gem = vgrad(y, cy - 5.6, cy + 5.6, c0 * 0.30 + 40, c0 * 0.12 + 20)
    over(im, gem, cover(rr - 5.7))
    hl = np.hypot(x - (cx - 1.6), y - (cy - 2.2))
    over(im, (255, 255, 255), cover(hl - 1.6) * (0.9 if on else 0.3))
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
