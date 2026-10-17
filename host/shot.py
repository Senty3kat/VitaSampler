"""Рисует экраны хост-превью в /tmp/shots/*.png: python3 shot.py"""
import subprocess, os
from PIL import Image
os.makedirs("/tmp/shots", exist_ok=True)
shots = [("edit", 0, 0, 0, 0), ("slice", 1, 1, 0, 0), ("grain", 2, 0, 0, 0), ("fx_filter", 3, 0, 0, 0),
         ("fx_crush", 3, 0, 1, 0), ("adv", 4, 0, 0, 0), ("help", 0, 0, 0, 1), ("empty", 0, 0, 0, 0, 1), ("rec", 0, 0, 0, 0, 2), ("pad", 5, 0, 0, 0, 0), ("pad_list", 5, 0, 0, 0, 3), ("pad_adsr", 5, 0, 0, 0, 4), ("pad_comp", 5, 0, 0, 0, 5), ("grain_key", 2, 0, 0, 0, 6)]
themed = [("aqua_edit", 0, 0, 0, 0, 0, 1), ("aqua_grain", 2, 0, 0, 0, 6, 1), ("aqua_pad", 5, 0, 0, 0, 0, 1), ("aqua_fx", 3, 0, 0, 0, 0, 1), ("aqua_settings", 0, 0, 0, 0, 0, 1, 1), ("classic_settings", 0, 0, 0, 0, 0, 0, 1)]
for name, page, mode, sec, leg, *st in shots:
    subprocess.run(["./preview.bin", "/tmp/frame.raw", str(page), str(mode), str(sec), str(leg), str(st[0] if st else 0)], check=True)
    Image.frombuffer("RGBA", (960, 544), open("/tmp/frame.raw", "rb").read(), "raw", "RGBA", 0, 1).convert("RGB").save(f"/tmp/shots/{name}.png")
for name, page, mode, sec, leg, sc, th, *ss in themed:
    subprocess.run(["./preview.bin", "/tmp/frame.raw", str(page), str(mode), str(sec), str(leg), str(sc), str(th), str(ss[0] if ss else 0)], check=True)
    Image.frombuffer("RGBA", (960, 544), open("/tmp/frame.raw", "rb").read(), "raw", "RGBA", 0, 1).convert("RGB").save(f"/tmp/shots/{name}.png")
# все экраны обеих тем для листа скина (tools/make_skin_sheet.py): /tmp/shots/sheet_<тема>_<имя>.png
for th in (0, 1):
    for name, page, mode, sec, leg, *st in shots:
        subprocess.run(["./preview.bin", "/tmp/frame.raw", str(page), str(mode), str(sec), str(leg), str(st[0] if st else 0), str(th), "0"], check=True)
        Image.frombuffer("RGBA", (960, 544), open("/tmp/frame.raw", "rb").read(), "raw", "RGBA", 0, 1).convert("RGB").save(f"/tmp/shots/sheet_{th}_{name}.png")
    subprocess.run(["./preview.bin", "/tmp/frame.raw", "0", "0", "0", "0", "0", str(th), "1"], check=True)
    Image.frombuffer("RGBA", (960, 544), open("/tmp/frame.raw", "rb").read(), "raw", "RGBA", 0, 1).convert("RGB").save(f"/tmp/shots/sheet_{th}_settings.png")
print("ok")
