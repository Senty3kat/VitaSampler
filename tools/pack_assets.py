#!/usr/bin/env python3
"""Собирает листы assets/knobs.png, keys.png, lamps.png и body.png из отдельных файлов design/<группа>/*.png
(обратно к tools/export_design.py). Размеры файлов должны совпадать с исходными."""
import os, shutil
from PIL import Image
ROOT = os.path.join(os.path.dirname(__file__), "..")
A = os.path.join(ROOT, "assets")
D = os.path.join(ROOT, "design")
KNOB_R = [20, 26, 28, 30]
KBOX, KS, LS = 88, 52, 40
STY = ["cream", "orange", "green", "grey"]
LCOL = ["red", "green", "amber"]


def put(sheet, path, x, y, w, h):
    im = Image.open(path).convert("RGBA")
    if im.size != (w, h):
        raise SystemExit(f"{path}: размер {im.size}, нужен {(w, h)}")
    sheet.paste(im, (x, y))


def main():
    k = Image.new("RGBA", (3 * KBOX, 4 * KBOX))
    for i, R in enumerate(KNOB_R):
        for j, n in enumerate(["idle", "active", "pointer"]):
            put(k, f"{D}/knobs/knob_R{R}_{n}.png", j * KBOX, i * KBOX, KBOX, KBOX)
    k.save(f"{A}/knobs.png", optimize=True)
    ky = Image.new("RGBA", (4 * KS, 2 * KS))
    for c, s in enumerate(STY):
        put(ky, f"{D}/keys/key_{s}_up.png", c * KS, 0, KS, KS)
        put(ky, f"{D}/keys/key_{s}_down.png", c * KS, KS, KS, KS)
    ky.save(f"{A}/keys.png", optimize=True)
    lp = Image.new("RGBA", (6 * LS, LS))
    for c, s in enumerate(LCOL):
        put(lp, f"{D}/lamps/lamp_{s}_off.png", (c * 2) * LS, 0, LS, LS)
        put(lp, f"{D}/lamps/lamp_{s}_on.png", (c * 2 + 1) * LS, 0, LS, LS)
    lp.save(f"{A}/lamps.png", optimize=True)
    b = f"{D}/background/body.png"
    if os.path.exists(b):
        if Image.open(b).size != (960, 544):
            raise SystemExit("body.png должен быть 960x544")
        shutil.copy(b, f"{A}/body.png")
    print("ok")


if __name__ == "__main__":
    main()
