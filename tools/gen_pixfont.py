#!/usr/bin/env python3
"""Пиксельные шрифты для темы Aqua: fonts/pix9, pix11, pix13.
DejaVu Sans Condensed Bold растрируется без сглаживания (hinting, 1 бит), каждый включённый пиксель становится квадратом 100x100 единиц,
em = N*100 единиц: при размере N пикселей глиф ложится ровно на сетку (при 2N - с пикселем 2x2). Запуск: python3 tools/gen_pixfont.py"""
import os
from PIL import ImageFont
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

HERE = os.path.dirname(__file__)
SRC = os.path.join(HERE, "..", "fonts", "DejaVuSansCondensed-Bold.ttf")
CHARS = [chr(c) for c in range(32, 127)] + ["°", "±", "·"]


def build(n, out, face_px):
    font = ImageFont.truetype(SRC, face_px)
    glyphs, metrics, cmap = {}, {}, {}
    pen = TTGlyphPen(None)
    glyphs[".notdef"] = pen.glyph(); metrics[".notdef"] = (n * 50, 0)
    asc = int(round(font.getmetrics()[0]))
    for ch in CHARS:
        name = "uni%04X" % ord(ch)
        adv = max(1, int(round(font.getlength(ch))))
        pen = TTGlyphPen(None)
        if ch != " ":
            mask, off = font.getmask2(ch, mode="1", anchor="ls")
            w, h = mask.size
            px = mask.load() if hasattr(mask, "load") else None
            data = mask  # ImagingCore: индексируется mask[x + y*w]? используем getpixel через im
            for y in range(h):
                for x in range(w):
                    if mask.getpixel((x, y)):
                        gx = (off[0] + x) * 100
                        gy = -(off[1] + y) * 100          # baseline = 0, вверх положительный
                        # квадрат по часовой стрелке (внешний контур TrueType)
                        pen.moveTo((gx, gy - 100)); pen.lineTo((gx, gy)); pen.lineTo((gx + 100, gy)); pen.lineTo((gx + 100, gy - 100)); pen.closePath()
        glyphs[name] = pen.glyph(); metrics[name] = (adv * 100, 0); cmap[ord(ch)] = name
    fb = FontBuilder(n * 100, isTTF=True)
    order = [".notdef"] + ["uni%04X" % ord(c) for c in CHARS]
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=asc * 100, descent=-int(n * 0.28) * 100)
    fb.setupNameTable({"familyName": "PixAqua%d" % n, "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=asc * 100, sTypoDescender=-int(n * 0.28) * 100, usWinAscent=asc * 100, usWinDescent=int(n * 0.28) * 100)
    fb.setupPost()
    fb.save(out)
    print(out, "asc", asc)


for n in (9, 11, 13):
    build(n, os.path.join(HERE, "..", "fonts", "pix%d.ttf" % n), n)
