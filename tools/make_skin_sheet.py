#!/usr/bin/env python3
"""Лист-схема скина для художника: assets/skin_sheet_classic.png и skin_sheet_aqua.png.
Сверху - атлас (корпус, ручки, кнопки, лампочки) с подписями В ПОЛЯХ, не на картинках; ниже - детали, которые пока рисует код
(цифры, рамка, стекло, шкала, значки...); в самом низу - общий вид всех экранов темы.
Перед запуском: make -C host preview && make -C host export (кладут экраны в /tmp/shots и детали в /tmp/parts_N).
Запуск: python3 tools/make_skin_sheet.py"""
import os, glob
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(__file__)
A = os.path.join(HERE, "..", "assets")
W = 1760
BG = (34, 36, 44); PANEL = (52, 55, 66); TXT = (236, 238, 244); DIM = (150, 156, 172); ACC = (255, 200, 80)
F = lambda n: ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", n)
FB = lambda n: ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", n)
SCREENS = [("edit", "EDIT"), ("slice", "SLICE"), ("grain", "GRAIN"), ("fx_filter", "FX filter"), ("fx_crush", "FX crush"), ("adv", "ADV"),
           ("pad", "PAD"), ("pad_adsr", "PAD ADSR"), ("pad_comp", "PAD COMP"), ("pad_list", "PAD список WAV"), ("settings", "Настройки"), ("help", "Справка"),
           ("empty", "Пусто"), ("rec", "Запись")]


def checker(w, h, s=8):
    im = Image.new("RGB", (w, h), (70, 72, 84))
    d = ImageDraw.Draw(im)
    for y in range(0, h, s):
        for x in range(0, w, s):
            if (x // s + y // s) % 2: d.rectangle((x, y, x + s - 1, y + s - 1), fill=(86, 88, 100))
    return im


def paste_a(sheet, im, xy):
    bg = checker(im.width, im.height)
    bg.paste(im, (0, 0), im)
    sheet.paste(bg, xy)


def build(theme, name):
    atlas = Image.open(os.path.join(A, f"skin_{theme}.png")).convert("RGBA")
    def cut(x, y, w, h): return atlas.crop((x, y, x + w, y + h))
    parts = []
    for p in sorted(glob.glob(f"/tmp/parts_{theme}/*.png")):
        parts.append((os.path.basename(p)[:-4], Image.open(p).convert("RGBA")))
    # ---- раскладка ----
    H0 = 1500
    sheet = Image.new("RGB", (W, 6000), BG)
    d = ImageDraw.Draw(sheet)
    d.text((24, 14), f"Vita Sampler - скин «{name}»   (атлас assets/skin_{theme}.png 1280x640 + детали, которые рисует код + общий вид)", font=FB(22), fill=ACC)
    y = 56
    d.text((24, y), "1. АТЛАС (лежит в игре как одна картинка skin_%d.png; ниже те же детали, разведённые для удобства; подписи - в полях)" % theme, font=FB(16), fill=TXT)
    y += 34
    # body
    d.text((24, y), "body 960x544 @(0,0) - корпус и панели; поверх рисуются ЭЛТ-окно, стекло, шкала, ручки, кнопки", font=F(13), fill=DIM)
    paste_a(sheet, cut(0, 0, 960, 544), (24, y + 20))
    # правая колонка: knobs
    kx = 24 + 960 + 40
    d.text((kx, y), "knobs 264x352 @(976,0): 4 строки (R20 R26 R28 R30) x 3 колонки", font=F(13), fill=DIM)
    ky = y + 20 + 26
    for i, t in enumerate(("покой", "активна", "риска (вверх)")):
        d.text((kx + 60 + i * 88 + 44 - d.textlength(t, font=F(12)) / 2, ky - 18), t, font=F(12), fill=TXT)
    for i, r in enumerate(("R20", "R26", "R28", "R30")):
        d.text((kx, ky + i * 88 + 38), r, font=F(12), fill=TXT)
    paste_a(sheet, cut(976, 0, 264, 352), (kx + 56, ky))
    # keys
    ly = ky + 352 + 24
    d.text((kx, ly), "keys 208x104 @(976,368): 9-slice, край 22 px, середина 8x8 однородная", font=F(13), fill=DIM)
    ky2 = ly + 44
    for i, t in enumerate(("cream", "orange", "green", "grey")):
        d.text((kx + 56 + i * 52 + 26 - d.textlength(t, font=F(12)) / 2, ky2 - 18), t, font=F(12), fill=TXT)
    d.text((kx, ky2 + 20), "up", font=F(12), fill=TXT); d.text((kx, ky2 + 72), "down", font=F(12), fill=TXT)
    paste_a(sheet, cut(976, 368, 208, 104), (kx + 56, ky2))
    # lamps
    ly2 = ky2 + 104 + 24
    d.text((kx, ly2), "lamps 240x40 @(976,488): красная REC, зелёная PLAY, янтарная LOOP; выкл / вкл", font=F(13), fill=DIM)
    paste_a(sheet, cut(976, 488, 240, 40), (kx, ly2 + 22))
    for i, t in enumerate(("red off", "red on", "green off", "green on", "amber off", "amber on")):
        d.text((kx + i * 40 + 20 - d.textlength(t, font=F(9)) / 2, ly2 + 66 + (i % 2) * 12), t, font=F(9), fill=TXT)
    y = max(y + 20 + 544, ly2 + 90) + 30
    # ---- детали кода ----
    d.text((24, y), "2. ДЕТАЛИ, КОТОРЫЕ ПОКА РИСУЕТ КОД (нет в атласе; их родной размер и подпись под каждой)", font=FB(16), fill=TXT)
    y += 30
    parts.sort(key=lambda p: -p[1].height)
    x = 24; row_h = 0
    for nm, im in parts:
        label = f"{nm} {im.width}x{im.height}"
        cw = max(im.width, int(d.textlength(label, font=F(11)))) + 18
        if x + cw > W - 24:
            x = 24; y += row_h + 14; row_h = 0
        paste_a(sheet, im, (x, y))
        d.text((x, y + im.height + 3), label, font=F(11), fill=DIM)
        x += cw
        row_h = max(row_h, im.height + 20)
    y += row_h + 30
    # ---- общий вид ----
    d.text((24, y), "3. ОБЩИЙ ВИД ВСЕХ ЭКРАНОВ ТЕМЫ (как выглядит приложение целиком, 960x544 уменьшено вдвое)", font=FB(16), fill=TXT)
    y += 30
    tw, th = 480, 272
    cols = 3
    gap = (W - 48 - cols * tw) // (cols - 1)
    for i, (key, label) in enumerate(SCREENS):
        p = f"/tmp/shots/sheet_{theme}_{key}.png"
        if not os.path.exists(p): continue
        cx = 24 + (i % cols) * (tw + gap); cy = y + (i // cols) * (th + 34)
        sheet.paste(Image.open(p).convert("RGB").resize((tw, th), Image.LANCZOS), (cx, cy))
        d.text((cx, cy + th + 4), label, font=F(13), fill=TXT)
    rows = (len(SCREENS) + cols - 1) // cols
    y += rows * (th + 34) + 10
    sheet = sheet.crop((0, 0, W, y))
    out = os.path.join(A, f"skin_sheet_{name.lower()}.png")
    sheet.save(out, optimize=True)
    print(out, sheet.size)


build(0, "Classic")
build(1, "Aqua")
