#!/usr/bin/env python3
"""Генератор PNG-спрайтов интерфейса (ручки, клавиши, лампочки): рисуется с 4-кратной супервыборкой.
Запуск: python3 tools/gen_assets.py   ->  assets/knobs.png, assets/keys.png, assets/lamps.png
Раскладка файлов должна совпадать с константами SK_* в main.c."""
import numpy as np
from PIL import Image
import os

SS = 4
OUT = os.path.join(os.path.dirname(__file__), "..", "assets")


def grid(w, h):
    ys, xs = np.mgrid[0:h * SS, 0:w * SS]
    return (xs + 0.5) / SS, (ys + 0.5) / SS


def new(w, h):
    return np.zeros((h * SS, w * SS, 4), np.float32)


def over(dst, rgb, a):
    """обычное наложение: rgb (H,W,3) 0..255, a (H,W) 0..1"""
    a = np.clip(a, 0, 1)[..., None]
    da = dst[..., 3:4]
    oa = a + da * (1 - a)
    dst[..., :3] = (rgb * a + dst[..., :3] * da * (1 - a)) / np.maximum(oa, 1e-6)
    dst[..., 3:4] = oa


def cover(d):
    """покрытие по расстоянию со знаком (в пикселях итога): <0 внутри"""
    return np.clip(0.5 - d * SS, 0, 1)


def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


def lerp(c0, c1, t):
    t = np.asarray(t)[..., None] if np.ndim(t) else t
    return np.asarray(c0, np.float32) * (1 - t) + np.asarray(c1, np.float32) * t


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
    out = np.concatenate([rgb, a * 255], -1)
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


# ---------------------------------------------------------------- ручки
KNOB_R = [20, 26, 28, 30]
KBOX = 88            # клетка спрайта ручки: центр в (44, 44)


def knob_body(R, active):
    S = KBOX
    img = new(S, S)
    x, y = grid(S, S)
    x -= S / 2
    y -= S / 2
    r = np.hypot(x, y)
    th = np.arctan2(y, x)
    # тень
    d = np.hypot(x, y - 3.0)
    over(img, np.full(img.shape[:2] + (3,), (60, 42, 24), np.float32), 0.34 * np.clip((R + 4.8 - d) / 7.0, 0, 1) ** 1.6)
    # ободок: литое кольцо, свет сверху-слева
    light = 0.5 + 0.5 * np.cos(th + 2.356)
    if active:
        rim = lerp((150, 54, 14), (252, 168, 84), light)
    else:
        rim = lerp((84, 78, 64), (240, 236, 224), light ** 1.2)
    over(img, rim, cover(r - (R + 3.3)))
    over(img, np.full(img.shape[:2] + (3,), (64, 56, 44), np.float32), 0.85 * cover(np.abs(r - (R + 0.1)) - 0.45))   # тёмный зазор
    # колпачок: радиальный градиент + «шлифованный металл»
    u = (x + 0.30 * R) / R
    v = (y + 0.36 * R) / R
    t = np.clip(np.hypot(u, v) / 1.35, 0, 1)
    cap = lerp((254, 251, 240), (172, 166, 150), t ** 1.15)
    spin = np.maximum(0, np.cos(2 * (th + 0.75))) ** 7
    cap = cap + 22 * spin[..., None] * smooth(0.12 * R, 0.5 * R, r)[..., None] - 10 * (np.maximum(0, np.cos(2 * (th - 0.8))) ** 9)[..., None] * smooth(0.2 * R, R, r)[..., None]
    cap = cap + 3.0 * np.sin(r * 6.5)[..., None]
    over(img, cap, cover(r - (R - 0.2)))
    # канавка на колпачке и светлая кромка под ней
    over(img, np.full(img.shape[:2] + (3,), (90, 80, 62), np.float32), 0.32 * cover(np.abs(r - 0.58 * R) - 0.5))
    over(img, np.full(img.shape[:2] + (3,), (255, 255, 255), np.float32), 0.45 * cover(np.abs(r - 0.58 * R - 0.9) - 0.4) * (light > 0.35))
    # тонкая внешняя кромка колпачка
    over(img, np.full(img.shape[:2] + (3,), (104, 96, 78), np.float32), 0.7 * cover(np.abs(r - (R - 0.3)) - 0.55))
    # блик
    hl = np.hypot(x + 0.36 * R, y + 0.44 * R)
    over(img, np.full(img.shape[:2] + (3,), (255, 255, 255), np.float32), 0.30 * np.clip(1 - hl / (0.42 * R), 0, 1) ** 1.5)
    return down(img)


def knob_pointer(R):
    S = KBOX
    img = new(S, S)
    x, y = grid(S, S)
    x -= S / 2
    y -= S / 2
    w, y0, y1 = 3.4, -0.88 * R, -0.30 * R
    sd = rrect_sd(x, y, -w / 2, y0, w, y1 - y0, w / 2)
    over(img, np.full(img.shape[:2] + (3,), (255, 255, 255), np.float32), 0.55 * cover(rrect_sd(x - 0.7, y - 1.1, -w / 2, y0, w, y1 - y0, w / 2)))   # светлая «подсветка» снизу-справа
    body = lerp((96, 76, 60), (62, 46, 36), (x + w / 2) / w)
    over(img, body, cover(sd))
    return down(img)


def make_knobs():
    sheet = np.zeros((KBOX * len(KNOB_R), KBOX * 3, 4), np.uint8)
    for i, R in enumerate(KNOB_R):
        sheet[i * KBOX:(i + 1) * KBOX, 0:KBOX] = knob_body(R, False)
        sheet[i * KBOX:(i + 1) * KBOX, KBOX:2 * KBOX] = knob_body(R, True)
        sheet[i * KBOX:(i + 1) * KBOX, 2 * KBOX:3 * KBOX] = knob_pointer(R)
    Image.fromarray(sheet, "RGBA").save(os.path.join(OUT, "knobs.png"), optimize=True)


# ---------------------------------------------------------------- клавиши (9-срез 52x52, поля 16)
KS = 52
KEY_STYLES = {
    0: ((249, 245, 232), (219, 213, 195), (150, 142, 120), (170, 162, 140)),
    1: ((247, 152, 72), (233, 106, 38), (190, 76, 20), (150, 58, 12)),
    3: ((120, 206, 130), (76, 178, 96), (48, 120, 62), (40, 100, 52)),
    4: ((226, 224, 216), (188, 185, 176), (122, 116, 102), (134, 128, 114)),
}


def key_sprite(style, pressed):
    top, bot, edge, side = KEY_STYLES[style]
    img = new(KS, KS)
    x, y = grid(KS, KS)
    bx, by, bw, bh = 8.0, 5.0 + (2.0 if pressed else 0.0), 36.0, 36.0
    sdb = rrect_sd(x, y, bx, by, bw, bh, 3.0)
    # мягкая тень под клавишей
    sds = rrect_sd(x, y, bx - 1, by + (2.5 if pressed else 4.5), bw + 2, bh, 4.0)
    over(img, np.full(img.shape[:2] + (3,), (60, 42, 24), np.float32), (0.20 if pressed else 0.28) * np.clip(1 - sds / (4.0 if pressed else 6.0), 0, 1) ** 2 * (sds > -1))
    if not pressed:
        sdf = rrect_sd(x, y, bx, by + 3.0, bw, bh, 3.0)
        over(img, np.full(img.shape[:2] + (3,), side, np.float32), cover(sdf))
    g = np.clip((y - by) / bh, 0, 1)
    c0, c1 = (bot, top) if pressed else (top, bot)
    face = lerp(c0, c1, g)
    over(img, face, cover(sdb))
    # внутренняя фаска: светлая сверху-слева, тёмная снизу-справа
    inner = -sdb
    bev = np.where(y < by + bh * 0.5, 1.0, -1.0)
    over(img, np.full(img.shape[:2] + (3,), 255.0, np.float32), (0.16 if not pressed else 0.05) * (bev > 0) * cover(np.abs(inner - 1.6) - 0.6) * (inner > 0.6))
    over(img, np.zeros(img.shape[:2] + (3,), np.float32), (0.07) * (bev < 0) * cover(np.abs(inner - 1.6) - 0.6) * (inner > 0.6))
    over(img, np.full(img.shape[:2] + (3,), edge, np.float32), cover(np.abs(sdb) - 0.5))
    if not pressed:
        over(img, np.full(img.shape[:2] + (3,), (255, 255, 255), np.float32), (0.36 if style == 0 else 0.30) * cover(rrect_sd(x, y, bx + 3, by + 1.0, bw - 6, 1.3, 0.6)))
    out = down(img)
    rng = np.random.default_rng(7 + style * 3 + pressed)          # чуть-чуть зерна пластика
    n = rng.normal(0, 1.4, out.shape[:2])[..., None]
    out[..., :3] = np.clip(out[..., :3].astype(np.float32) + n * (out[..., 3:4] > 200), 0, 255).astype(np.uint8)
    return out


def make_keys():
    sheet = np.zeros((KS * 2, KS * 4, 4), np.uint8)
    for ci, st in enumerate([0, 1, 3, 4]):
        for p in (0, 1):
            sheet[p * KS:(p + 1) * KS, ci * KS:(ci + 1) * KS] = key_sprite(st, p)
    Image.fromarray(sheet, "RGBA").save(os.path.join(OUT, "keys.png"), optimize=True)


# ---------------------------------------------------------------- лампочки 32x32
LS = 40
LAMPS = [(255, 52, 38), (96, 220, 110), (255, 170, 30)]


def lamp(col, on, r=6.0):
    col = np.asarray(col, np.float32)
    img = new(LS, LS)
    x, y = grid(LS, LS)
    x -= LS / 2
    y -= LS / 2
    rr = np.hypot(x, y)
    th = np.arctan2(y, x)
    if on:
        over(img, np.full(img.shape[:2] + (3,), col, np.float32), 0.36 * np.exp(-(rr / (r * 1.45)) ** 2.0) * (1 - smooth(13.0, 19.0, rr)))
        over(img, np.full(img.shape[:2] + (3,), col, np.float32), 0.20 * np.exp(-(rr / (r * 2.2)) ** 2.0) * (1 - smooth(13.0, 19.0, rr)))
    over(img, np.full(img.shape[:2] + (3,), (30, 20, 12), np.float32), 0.22 * np.clip((r + 4.2 - np.hypot(x, y - 1.4)) / 3.0, 0, 1) * (rr > r))
    # оправа: тёмный металл со светом сверху-слева
    light = 0.5 + 0.5 * np.cos(th + 2.356)
    over(img, lerp((40, 31, 25), (140, 126, 108), light ** 1.4), cover(rr - (r + 2.7)))
    over(img, np.full(img.shape[:2] + (3,), (26, 19, 14), np.float32), cover(rr - (r + 1.9)))
    rim = lerp(col, (50, 40, 34), 0.0 if on else 0.5)
    over(img, rim, cover(rr - (r + 1.6)) * (1 - cover(rr - (r + 0.5))))
    over(img, np.full(img.shape[:2] + (3,), (24, 17, 12), np.float32), cover(rr - (r + 0.5)))
    # купол
    u = (x + 0.30 * r) / r
    v = (y + 0.36 * r) / r
    t = np.clip(np.hypot(u, v) / 1.35, 0, 1)
    if on:
        dome = lerp(lerp(col, (255, 255, 255), 0.62), col * 0.55, t ** 0.9)
    else:
        dome = lerp(lerp(col * 0.35, (90, 80, 72), 0.4), col * 0.10 + 12, t ** 0.8)
    over(img, dome, cover(rr - r))
    # отражение от нижней кромки
    bounce = np.clip(1 - np.hypot(x - 0.1 * r, y - 0.78 * r) / (0.55 * r), 0, 1) ** 2
    over(img, np.full(img.shape[:2] + (3,), col if on else (150, 130, 110), np.float32), (0.42 if on else 0.12) * bounce * cover(rr - r))
    # блик
    sd = np.hypot((x + 0.30 * r) / 0.58, (y + 0.40 * r) / 0.34) - r
    over(img, np.full(img.shape[:2] + (3,), (255, 255, 255), np.float32), (0.90 if on else 0.42) * cover(sd / 1.0) * cover(rr - r * 0.98))
    return down(img)


def make_lamps():
    sheet = np.zeros((LS, LS * 6, 4), np.uint8)
    for ci, col in enumerate(LAMPS):
        for on in (0, 1):
            sheet[:, (ci * 2 + on) * LS:(ci * 2 + on + 1) * LS] = lamp(col, on)
    Image.fromarray(sheet, "RGBA").save(os.path.join(OUT, "lamps.png"), optimize=True)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    make_knobs()
    make_keys()
    make_lamps()
    print("ok")
