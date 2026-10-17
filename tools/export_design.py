#!/usr/bin/env python3
"""Раскладывает листы спрайтов assets/*.png на отдельные файлы design/<группа>/*.png (для художника).
Обратная сборка: tools/pack_assets.py. Процедурные детали выгружает `make -C host export` (design/parts/)."""
import os, shutil
from PIL import Image
ROOT = os.path.join(os.path.dirname(__file__), "..")
A = os.path.join(ROOT, "assets")
D = os.path.join(ROOT, "design")
KNOB_R = [20, 26, 28, 30]
KBOX, KS, LS = 88, 52, 40
STY = ["cream", "orange", "green", "grey"]
LCOL = ["red", "green", "amber"]


def cut(img, x, y, w, h, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.crop((x, y, x + w, y + h)).save(path)


def main():
    k = Image.open(os.path.join(A, "knobs.png")).convert("RGBA")
    for i, R in enumerate(KNOB_R):
        cut(k, 0, i * KBOX, KBOX, KBOX, f"{D}/knobs/knob_R{R}_idle.png")
        cut(k, KBOX, i * KBOX, KBOX, KBOX, f"{D}/knobs/knob_R{R}_active.png")
        cut(k, 2 * KBOX, i * KBOX, KBOX, KBOX, f"{D}/knobs/knob_R{R}_pointer.png")
    ky = Image.open(os.path.join(A, "keys.png")).convert("RGBA")
    for c, s in enumerate(STY):
        cut(ky, c * KS, 0, KS, KS, f"{D}/keys/key_{s}_up.png")
        cut(ky, c * KS, KS, KS, KS, f"{D}/keys/key_{s}_down.png")
    lp = Image.open(os.path.join(A, "lamps.png")).convert("RGBA")
    for c, s in enumerate(LCOL):
        cut(lp, (c * 2) * LS, 0, LS, LS, f"{D}/lamps/lamp_{s}_off.png")
        cut(lp, (c * 2 + 1) * LS, 0, LS, LS, f"{D}/lamps/lamp_{s}_on.png")
    os.makedirs(f"{D}/background", exist_ok=True)
    shutil.copy(os.path.join(A, "body.png"), f"{D}/background/body.png")


if __name__ == "__main__":
    main()
