#!/usr/bin/env python3
"""Собирает ОДНУ большую текстуру скина (как скин в Minecraft): assets/skin_0.png (Classic) и assets/skin_1.png (Aqua),
плюс подписанную схему assets/skin_guide.png. Раскладка (x, y, w, h) должна совпадать с SKIN_* в main.c.
Исходные листы: Classic - body/knobs/keys/lamps.png, Aqua - t1_*.png (knobs у Aqua перекрашиваются в main.c, поэтому здесь
берётся t1_knobs.png, если он есть, иначе knobs.png перекрашенный так же, как th_map). Запуск: python3 tools/pack_skin.py"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(__file__)
A = os.path.join(HERE, "..", "assets")
SIZE = (1280, 640)
# часть: (x, y, w, h) - между частями зазор 16 px, свободное место справа внизу оставлено под новые детали
LAYOUT = {
    "body":  (0, 0, 960, 544),
    "knobs": (976, 0, 264, 352),
    "keys":  (976, 368, 208, 104),
    "lamps": (976, 488, 240, 40),
}
NAMES = {0: {"body": "body.png", "knobs": "knobs.png", "keys": "keys.png", "lamps": "lamps.png"},
         1: {"body": "t1_body.png", "knobs": "t1_knobs.png", "keys": "t1_keys.png", "lamps": "t1_lamps.png"}}


def load(theme, part):
    p = os.path.join(A, NAMES[theme][part])
    if not os.path.exists(p):
        p = os.path.join(A, NAMES[0][part])
    return Image.open(p).convert("RGBA")


for theme in (0, 1):
    atlas = Image.new("RGBA", SIZE, (0, 0, 0, 0))
    for part, (x, y, w, h) in LAYOUT.items():
        im = load(theme, part)
        assert im.size == (w, h), (part, im.size)
        atlas.paste(im, (x, y))
    atlas.save(os.path.join(A, f"skin_{theme}.png"), optimize=True)
    if theme == 0:
        g = Image.new("RGBA", SIZE, (40, 40, 48, 255))
        g.alpha_composite(atlas)
        dr = ImageDraw.Draw(g)
        for part, (x, y, w, h) in LAYOUT.items():
            dr.rectangle((x, y, x + w - 1, y + h - 1), outline=(255, 60, 60, 255))
            dr.text((x + 4, y + 4), f"{part} {w}x{h} @({x},{y})", fill=(255, 255, 0, 255))
        g.convert("RGB").save(os.path.join(A, "skin_guide.png"))
print("skin_0.png skin_1.png skin_guide.png")
