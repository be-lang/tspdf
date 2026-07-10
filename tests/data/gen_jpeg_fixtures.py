# Deterministic JPEG fixtures for the jpeg_codec decoder tests, plus PIL's
# decoded pixels as raw dumps (*.raw) so the C tests can compare our decoder
# against an independent implementation (libjpeg-turbo via Pillow) pixel by
# pixel, offline. Run with:
#   uv run --python 3.12 --with pillow python tests/data/gen_jpeg_fixtures.py
# The 1x2-vertical-sampling fixture additionally needs cjpeg (libjpeg-turbo)
# on PATH, because Pillow can only emit 4:4:4 / 4:2:2 / 4:2:0.
from PIL import Image
import io
import math
import os
import shutil
import subprocess

out = "tests/data"

_seed = [0]


def rnd():
    # LCG, same shape as gen_img_fixtures.py: deterministic across runs.
    _seed[0] = (_seed[0] * 1103515245 + 12345) & 0x7FFFFFFF
    return _seed[0] >> 16


def photo_rgb(w, h):
    # Smooth waves + mild noise: "photo-like" (non-trivial AC energy, no
    # hard synthetic edges that would exaggerate ringing differences).
    _seed[0] = 20250701
    data = []
    for y in range(h):
        for x in range(w):
            n = rnd() & 7
            r = int(96 + 80 * math.sin(x * 0.11) + 40 * math.cos(y * 0.07)) + n
            g = int(120 + 60 * math.sin((x + y) * 0.05)) + (n >> 1)
            b = int(128 + 90 * math.cos(x * 0.04 + y * 0.09)) + n
            data.append((max(0, min(255, r)), max(0, min(255, g)),
                         max(0, min(255, b))))
    return data


def gray_gradient(w, h):
    return [min(255, (x * 3 + y * 2) % 256) for y in range(h) for x in range(w)]


def gray_noise(w, h):
    _seed[0] = 424242
    return [max(0, min(255, 128 + int(60 * math.sin(x * 0.13 + y * 0.06)) +
                       (rnd() & 15))) for y in range(h) for x in range(w)]


def flat_blocks(w, h):
    colors = [(200, 40, 40), (40, 200, 40), (40, 40, 200), (220, 220, 60),
              (128, 128, 128), (255, 255, 255), (0, 0, 0), (60, 180, 220)]
    return [colors[((x // 16) + (y // 16) * 4) % len(colors)]
            for y in range(h) for x in range(w)]


def save_with_raw(name, im, **kw):
    path = f"{out}/{name}.jpg"
    im.save(path, "JPEG", **kw)
    dump_raw(name)


def dump_raw(name):
    # PIL's decode (libjpeg-turbo, fancy upsampling) is the oracle the C
    # tests compare against.
    dec = Image.open(f"{out}/{name}.jpg")
    dec = dec.convert("L" if dec.mode == "L" else "RGB")
    with open(f"{out}/{name}.raw", "wb") as f:
        f.write(dec.tobytes())


W, H = 64, 48

img = Image.new("L", (W, H))
img.putdata(gray_gradient(W, H))
save_with_raw("jpg_gray_grad", img, quality=90)

img = Image.new("L", (W, H))
img.putdata(gray_noise(W, H))
save_with_raw("jpg_gray_noise", img, quality=85)

photo = Image.new("RGB", (W, H))
photo.putdata(photo_rgb(W, H))
save_with_raw("jpg_rgb_444", photo, quality=90, subsampling=0)
save_with_raw("jpg_rgb_422", photo, quality=85, subsampling=1)
save_with_raw("jpg_rgb_420", photo, quality=85, subsampling=2)
# Restart markers: DRI + RSTn every 2 MCU rows (Pillow >= 10.2).
save_with_raw("jpg_rgb_restart", photo, quality=85, subsampling=2,
              restart_marker_blocks=2)
# Progressive: our decoder must reject this cleanly (no .raw needed).
photo.save(f"{out}/jpg_progressive.jpg", "JPEG", quality=85, progressive=True)

# Odd dimensions: partial MCUs on both edges with 4:2:0.
ow, oh = 61, 37
odd = Image.new("RGB", (ow, oh))
odd.putdata(photo_rgb(ow, oh))
save_with_raw("jpg_rgb_420_odd", odd, quality=85, subsampling=2)

flat = Image.new("RGB", (W, H))
flat.putdata(flat_blocks(W, H))
save_with_raw("jpg_rgb_flat", flat, quality=75, subsampling=2)

# 1x2 vertical sampling (Y h1v2, a.k.a. 4:4:0): Pillow cannot emit it, cjpeg
# can (-sample 1x2). Skipped (fixture kept from a previous run) if absent.
if shutil.which("cjpeg"):
    ppm = io.BytesIO()
    photo.save(ppm, "PPM")
    jpg = subprocess.run(
        ["cjpeg", "-quality", "85", "-sample", "1x2"],
        input=ppm.getvalue(), stdout=subprocess.PIPE, check=True).stdout
    with open(f"{out}/jpg_rgb_1x2.jpg", "wb") as f:
        f.write(jpg)
    dump_raw("jpg_rgb_1x2")
else:
    print("warning: cjpeg not found, jpg_rgb_1x2.jpg not regenerated")

for f in sorted(os.listdir(out)):
    if f.startswith("jpg_"):
        print(f, os.path.getsize(os.path.join(out, f)))
