#!/usr/bin/env python3
"""Combine headless screenshots (PPM) into one labelled PNG contact sheet."""
import sys, glob, os
from PIL import Image, ImageDraw
files = sorted(glob.glob(sys.argv[1]))
out = sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 5
w, h = 320, 224
rows = (len(files) + cols - 1) // cols
sheet = Image.new('RGB', (cols * w, rows * (h + 14)), (40, 40, 40))
d = ImageDraw.Draw(sheet)
for i, f in enumerate(files):
    im = Image.open(f)
    x, y = (i % cols) * w, (i // cols) * (h + 14)
    sheet.paste(im, (x, y + 14))
    d.text((x + 4, y + 1), os.path.basename(f), fill=(255, 255, 0))
sheet.save(out)
