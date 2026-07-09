# Deterministic PNG fixtures for img2pdf tests (gradient + LCG noise so the
# files are a few KB and the 1.2x passthrough size assertion is meaningful).
from PIL import Image
import os

W, H = 80, 60
out = "tests/data"

_seed = [12345]
def rnd():
    _seed[0] = (_seed[0] * 1103515245 + 12345) & 0x7fffffff
    return _seed[0] >> 16

def px(x, y):
    n = rnd() & 15
    r = (x * 2 + n) % 256
    g = (y * 3 + (n >> 1)) % 256
    b = (x + y + n) % 256
    return (r, g, b)

_seed[0] = 12345
rgb_data = [px(x, y) for y in range(H) for x in range(W)]

im = Image.new("RGB", (W, H)); im.putdata(rgb_data)
im.save(f"{out}/img_rgb.png", optimize=True)

_seed[0] = 999
img = Image.new("L", (W, H))
img.putdata([(x * 2 + y + (rnd() & 15)) % 256 for y in range(H) for x in range(W)])
img.save(f"{out}/img_gray.png", optimize=True)

imp = im.convert("P", palette=Image.ADAPTIVE, colors=64)
imp.save(f"{out}/img_palette.png")

imp4 = im.convert("P", palette=Image.ADAPTIVE, colors=16)
imp4.save(f"{out}/img_palette4.png", bits=4)

imp2 = im.convert("P", palette=Image.ADAPTIVE, colors=32)
imp2.save(f"{out}/img_palette_trns.png", transparency=bytes(range(0, 256, 8)))

_seed[0] = 4242
ima = Image.new("RGBA", (W, H))
ima.putdata([rgb_data[y * W + x] + (((x + y) * 2 + (rnd() & 15)) % 256,)
             for y in range(H) for x in range(W)])
ima.save(f"{out}/img_rgba.png", optimize=True)

_seed[0] = 777
imga = Image.new("LA", (W, H))
imga.putdata([((x * 2 + y + (rnd() & 15)) % 256, (x * 2) % 256)
              for y in range(H) for x in range(W)])
imga.save(f"{out}/img_gray_alpha.png")

im16 = Image.new("I;16", (W, H))
im16.putdata([((x * 517 + y * 311) % 65536) for y in range(H) for x in range(W)])
im16.save(f"{out}/img_gray16.png")

for f in sorted(os.listdir(out)):
    if f.endswith(".png"):
        print(f, os.path.getsize(os.path.join(out, f)))

# Pillow cannot write interlaced PNGs; build an Adam7 one by hand.
import struct, zlib
def _chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)

def write_interlaced(path, w, h, getpx):
    passes = [(0,0,8,8),(4,0,8,8),(0,4,4,8),(2,0,4,4),(0,2,2,4),(1,0,2,2),(0,1,1,2)]
    raw = bytearray()
    for x0, y0, dx, dy in passes:
        for y in range(y0, h, dy):
            row = bytearray()
            for x in range(x0, w, dx):
                row += bytes(getpx(x, y))
            if row:
                raw += b"\x00" + row
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 1)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(_chunk(b"IHDR", ihdr))
        f.write(_chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(_chunk(b"IEND", b""))

write_interlaced(f"{out}/img_interlaced.png", 32, 24,
                 lambda x, y: ((x * 3) % 256, (y * 4) % 256, (x + y) % 256))
print("img_interlaced.png", os.path.getsize(f"{out}/img_interlaced.png"))
