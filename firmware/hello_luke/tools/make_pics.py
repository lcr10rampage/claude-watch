#!/usr/bin/env python3
"""Convert photos into 410x502 raw RGB565 (little-endian) files for the watch's home face.
Usage: make_pics.py <src_dir> <out_dir> [exclude1 exclude2 ...]
Writes pic_00.bin ... and pics.txt (index -> source name). Excluded names are skipped (matched by filename).
Run with the system python (/usr/bin/python3), which has Pillow."""
import sys, glob, os
from PIL import Image, ImageOps
W, H = 410, 502
src, out = sys.argv[1], sys.argv[2]
exclude = {e.lower() for e in sys.argv[3:]}
os.makedirs(out, exist_ok=True)
for old in glob.glob(os.path.join(out, 'pic_*.bin')):
    os.remove(old)
files = sorted(f for f in glob.glob(os.path.join(src, '*'))
               if f.lower().endswith(('.jpg', '.jpeg', '.png'))
               and os.path.basename(f).lower() not in exclude
               and os.path.splitext(os.path.basename(f))[0].lower() not in exclude)
for e in exclude:
    print(f'excluding {e}')
index = []
for i, f in enumerate(files):
    im = ImageOps.exif_transpose(Image.open(f)).convert('RGB')
    im = ImageOps.fit(im, (W, H), method=Image.LANCZOS, centering=(0.5, 0.5))
    px = im.tobytes()                       # RGB888
    buf = bytearray(W * H * 2)
    j = 0
    for k in range(0, len(px), 3):
        r, g, b = px[k], px[k+1], px[k+2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        buf[j] = v & 0xFF; buf[j+1] = v >> 8   # little-endian, LVGL native RGB565
        j += 2
    name = f'pic_{i:02d}.bin'
    open(os.path.join(out, name), 'wb').write(buf)
    index.append(f'{i:02d} {os.path.basename(f)}')
    print(f'{name}  <- {os.path.basename(f)}')
open(os.path.join(out, 'pics.txt'), 'w').write('\n'.join(index) + '\n')
print(f'{len(files)} pictures, {len(files)*W*H*2/1024/1024:.1f} MB total')
