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
    """Ручка как на С1-65: многогранный корпус (10 граней, у каждой свой тон), металлический диск сверху, юбка из металла."""
    S = KBOX
    img = new(S, S)
    x, y = grid(S, S)
    x -= S / 2
    y -= S / 2
    r = np.hypot(x, y)
    th = np.arctan2(y, x)
    C = lambda c: np.full(img.shape[:2] + (3,), c, np.float32)
    d = np.hypot(x, y - 2.2)                                                       # тень: короткая и плотная
    over(img, C((40, 28, 16)), 0.42 * np.clip((R + 4.4 - d) / 3.2, 0, 1) ** 1.3)
    light = 0.5 + 0.5 * np.cos(th + 2.356)                                         # свет сверху-слева
    N = 10
    sec = 2 * np.pi / N
    off = 0.3
    ang = np.mod(th - off, 2 * np.pi)
    k = np.floor(ang / sec)
    loc = ang - k * sec - sec / 2
    def poly(Rc):
        return r * np.cos(loc) - Rc * np.cos(sec / 2)
    Rp = R + 2.2                                                                   # многогранный корпус (острые рёбра)
    # юбка — тоже многогранник, чуть шире корпуса
    if active:
        skirt = lerp((170, 62, 16), (255, 176, 92), light)
    else:
        skirt = lerp((110, 112, 114), (236, 238, 240), light ** 1.2)
    over(img, skirt, cover(poly(Rp + 1.5)))
    over(img, C((50, 50, 52)), 0.85 * cover(np.abs(poly(Rp + 1.5)) - 0.4))
    over(img, C((40, 38, 34)), 0.9 * cover(np.abs(poly(Rp - 0.1)) - 0.4))
    face_ang = off + (k + 0.5) * sec
    fl = 0.5 + 0.5 * np.cos(face_ang + 2.356)                                      # яркость грани: плоская на всю грань
    plastic = lerp((128, 126, 116), (232, 230, 216), fl ** 1.1)
    over(img, plastic, cover(poly(Rp - 0.6)))
    dpoly = poly(Rp - 0.6)
    ed = np.abs(loc) > sec / 2 - 0.03
    over(img, C((70, 68, 60)), 0.6 * ed * (r > 0.8 * R) * cover(dpoly + 0.2))
    over(img, C((255, 255, 255)), 0.25 * (np.abs(np.abs(loc) - (sec / 2 - 0.07)) < 0.02) * (r > 0.8 * R) * cover(dpoly + 0.2) * (fl > 0.5))
    # верхняя площадка пластика (фаска от граней) и диск из шлифованного металла
    ri = 0.80 * R
    over(img, lerp((214, 212, 200), (170, 168, 156), np.clip((r / ri), 0, 1) ** 2), cover(r - ri))
    over(img, C((82, 80, 72)), 0.7 * cover(np.abs(r - ri) - 0.45))
    rm = 0.66 * R
    sheen = np.maximum(0, np.cos(2 * (th + 0.8))) ** 3
    metal = lerp((176, 178, 180), (236, 238, 240), sheen) + 5 * np.sin(th * 48)[..., None]
    over(img, metal, cover(r - rm))
    over(img, C((70, 70, 72)), 0.85 * cover(np.abs(r - rm) - 0.45))
    over(img, C((255, 255, 255)), 0.55 * cover(np.abs(r - rm + 0.9) - 0.35) * (light > 0.5))
    return down(img)


def knob_pointer(R):
    """Тёмная риска-указатель (вращается вокруг центра)."""
    S = KBOX
    img = new(S, S)
    x, y = grid(S, S)
    x -= S / 2
    y -= S / 2
    w, y0, y1 = 2.6, -0.62 * R, -0.06 * R
    sdr = lambda dx, dy: rrect_sd(x - dx, y - dy, -w / 2, y0, w, y1 - y0, 0.6)
    over(img, np.full(img.shape[:2] + (3,), (255, 255, 255), np.float32), 0.7 * cover(sdr(0.5, 0.7)))
    over(img, np.full(img.shape[:2] + (3,), (34, 30, 28), np.float32), cover(sdr(0, 0)))
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
    """Объёмный кейкап 70-х: высокая трапеция, видны скошенные боковые и передняя стенки, вогнутая верхняя грань."""
    top, bot, edge, side = KEY_STYLES[style]
    img = new(KS, KS)
    x, y = grid(KS, KS)
    C = lambda c: np.full(img.shape[:2] + (3,), c, np.float32)
    off = 2.0 if pressed else 0.0
    bx, by, bw, bh = 8.0, 5.0 + off, 36.0, 36.0
    # гнездо: тёмная тень вокруг основания
    sds = rrect_sd(x, y, bx - 1.0, by - 0.5 + 1.2, bw + 2.0, bh + 1.0, 5.0)
    over(img, C((18, 14, 10)), 0.55 * np.clip(1 - sds / 2.2, 0, 1) ** 1.2 * (sds > -1.0))
    # основание кейкапа (внешний контур)
    sdo = rrect_sd(x, y, bx, by, bw, bh, 4.5)
    # верхняя грань: уже основания; снизу — высокая передняя стенка
    li, ri, ti = 5.5, 5.5, 3.5
    fw = 5.0 if pressed else 12.0
    tx0, ty0, tw, th_ = bx + li, by + ti, bw - li - ri, bh - ti - fw
    sdt = rrect_sd(x, y, tx0, ty0, tw, th_, 3.4)
    # стенки: ориентация определяется положением относительно верхней грани
    dark = lerp(top, (0, 0, 0), 0.30)
    mid_ = lerp(top, (0, 0, 0), 0.12)
    lite = lerp(top, (255, 255, 255), 0.35)
    gy = np.clip((y - (ty0 + th_ - 1.0)) / (fw + 1.0), 0, 1)       # 0 под верхней гранью, 1 у основания
    front = lerp(lerp(bot, (255, 255, 255), 0.06), dark, gy ** 0.8)
    below = y > ty0 + th_ - 1.5
    lit_zone = (y < ty0) | (x < tx0 + 1.0)
    wall = np.where(below[..., None], front, np.where(lit_zone[..., None], lite, mid_))
    over(img, wall, cover(sdo))
    # верхняя грань: вогнутая (чуть темнее в середине), вертикальный градиент
    g = np.clip((y - ty0) / th_, 0, 1)
    c0, c1 = (bot, top) if pressed else (lerp(top, (255, 255, 255), 0.10), bot)
    face = lerp(c0, c1, g)
    cx_ = tx0 + tw / 2
    nxx = np.clip(np.abs(x - cx_) / (tw / 2), 0, 1)
    dish = np.clip(1 - np.abs(g * 2 - 1) ** 2.0, 0, 1) * (1 - nxx ** 2)
    face = face * (1 - 0.13 * dish)[..., None]
    over(img, face, cover(sdt))
    over(img, C(edge), 0.45 * cover(np.abs(sdt) - 0.45))           # кромка верхней грани
    over(img, C((255, 255, 255)), 0.30 * cover(np.abs(sdt + 0.9) - 0.4) * (g < 0.5))   # светлая фаска сверху
    over(img, C(edge), cover(np.abs(sdo) - 0.5))
    if not pressed:                                                # резкая светлая грань на стыке верха и передней стенки
        over(img, C((255, 255, 255)), 0.55 * cover(rrect_sd(x, y, tx0 + 1.5, ty0 + th_ - 0.2, tw - 3, 1.0, 0.4)))
    out = down(img)
    rng = np.random.default_rng(7 + style * 3 + pressed)
    n = rng.normal(0, 1.3, out.shape[:2])[..., None]
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




# ---------------------------------------------------------------- корпус 960x544 (фон + врезанные панели + винты)
def make_body():
    W, H = 960, 544
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    px, py = xx + 0.5, yy + 0.5
    rng = np.random.default_rng(11)
    base = lerp((238, 234, 222), (221, 216, 202), py / H)
    base = base * np.ones((H, W, 1), np.float32)
    # верхняя планка чуть светлее
    base = base + (py < 40)[..., None] * 3.0
    # мягкая виньетка по краям корпуса
    vx = np.abs(px - W / 2) / (W / 2)
    vy = np.abs(py - H / 2) / (H / 2)
    base = base - 7.0 * (np.clip(np.maximum(vx, vy) - 0.78, 0, 1) / 0.22)[..., None] ** 1.5
    # зерно пластика (мелкое, тёплое)
    g = rng.normal(0, 1.2, (H, W)).astype(np.float32)
    base = base + g[..., None] * np.array([1.0, 1.0, 0.9], np.float32)
    img = np.concatenate([base, np.full((H, W, 1), 255, np.float32)], -1)

    def cov(d):
        return np.clip(0.5 - d, 0, 1)

    def blend(rgb, a):
        a = np.clip(a, 0, 1)[..., None]
        img[..., :3] = img[..., :3] * (1 - a) + np.asarray(rgb, np.float32) * a

    def inset_panel(x0, y0, w, h, r, top_c, bot_c):
        # светлая кромка снизу-справа, тёмная сверху-слева: панель врезана в корпус
        blend((255, 255, 255), 0.65 * cov(rrect_sd(px, py, x0, y0 + 1.2, w, h, r)))
        blend((110, 102, 84), 0.55 * cov(rrect_sd(px, py, x0 - 0.6, y0 - 0.9, w + 1.2, h, r)))
        sd = rrect_sd(px, py, x0, y0, w, h, r)
        t = np.clip((py - y0) / h, 0, 1)
        face = lerp(top_c, bot_c, t)
        face = face + rng.normal(0, 0.9, (H, W)).astype(np.float32)[..., None]
        blend(face, cov(sd))
        # внутренняя тень от верхнего и левого края
        inner = -sd
        blend((60, 52, 36), 0.26 * np.clip(1 - (py - y0) / 7.0, 0, 1) ** 1.6 * cov(sd))
        blend((60, 52, 36), 0.14 * np.clip(1 - (px - x0) / 4.0, 0, 1) ** 1.6 * cov(sd))
        blend((255, 255, 255), 0.30 * np.clip(1 - (y0 + h - py) / 2.5, 0, 1) ** 1.5 * cov(sd))
        blend((120, 112, 90), 0.9 * cov(np.abs(sd) - 0.5))

    inset_panel(10, 362, 938, 174, 6, (216, 210, 192), (198, 191, 172))
    inset_panel(896, 46, 56, 306, 6, (204, 198, 180), (190, 183, 164))

    # разделитель на панели управления
    blend((150, 142, 120), (np.abs(px - 292.5) < 0.5) * (py > 376) * (py < 522) * 1.0)
    blend((255, 255, 255), 0.55 * (np.abs(px - 293.5) < 0.5) * (py > 376) * (py < 522))

    def screw(cx, cy, r=5.0, ang=0.4):
        d = np.hypot(px - cx, py - cy)
        blend((0, 0, 0), 0.22 * np.clip(1 - np.hypot(px - cx, py - cy - 1.0) / (r + 1.6), 0, 1))
        light = np.clip(0.5 - 0.5 * ((px - cx) * 0.6 + (py - cy) * 0.8) / r, 0, 1)
        head = lerp((120, 118, 112), (232, 230, 222), light)
        blend(head, cov(d - r))
        blend((70, 66, 58), 0.9 * cov(np.abs(d - r) - 0.55))
        for a in (ang, ang + np.pi / 2):                           # крестовой шлиц
            ux, uy = np.cos(a), np.sin(a)
            along = (px - cx) * ux + (py - cy) * uy
            across = -(px - cx) * uy + (py - cy) * ux
            blend((50, 46, 40), 0.85 * cov(np.abs(across) - 0.7) * (np.abs(along) < r * 0.62) * cov(d - r + 0.8))

    for cx, cy in ((24, 375), (934, 375), (24, 523), (934, 523)):
        screw(cx, cy, 4.6, 0.35 + (cx * 7 + cy) % 5 * 0.4)

    out = np.clip(img + 0.5, 0, 255).astype(np.uint8)
    Image.fromarray(out, "RGBA").save(os.path.join(OUT, "body.png"), optimize=True)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    make_knobs()
    make_keys()
    make_lamps()
    make_body()
    print("ok")
