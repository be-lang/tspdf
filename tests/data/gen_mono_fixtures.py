# Regenerates the bilevel `compress --lossy` fixtures (tests/test_cli.sh)
# and the CCITT fuzz seed:
#
#   lossy_mono.pdf        a 600-dpi scanned-text-like page: a 900x900 G4
#                         (CCITTFaxDecode) image drawn at 1.5x1.5 inches.
#                         --lossy must downsample it to the 300-dpi default
#                         and shrink the file.
#   lossy_mono_low.pdf    the same content at 300 dpi (450x450 G4): at or
#                         below 1.3x the mono target, must pass through
#                         byte-identical.
#   lossy_mono_flate.pdf  450x450 at 300 dpi, stored as packed 1-bpp
#                         FlateDecode: converted to G4 1:1 (no downsample),
#                         so input and output renders match pixel-exactly.
#   ../../fuzz/corpus/reader/ccitt_g4.pdf
#                         a tiny (256x256, exactly the 65536-pixel gate) G4
#                         image PDF so the fuzzer exercises the CCITT
#                         decoder through the lossy pass.
#
# The bitmap is deterministic (fixed LCG) "text": word blobs of vertical
# strokes whose edges random-walk per scanline, like glyph outlines at scan
# resolution. G4 streams come from Pillow/libtiff (single TIFF strip; the
# image is inverted on the way in because Pillow writes MinIsBlack — see
# gen_ccitt_fixtures.py).
#
# Usage (from the repo root):
#   uv run --python 3.12 --with pillow python tests/data/gen_mono_fixtures.py

import io
import os
import struct
import zlib

from PIL import Image

OUT = os.path.dirname(os.path.abspath(__file__))
FUZZ = os.path.normpath(os.path.join(OUT, "..", "..", "fuzz", "corpus", "reader"))


class Lcg:
    def __init__(self, seed):
        self.s = seed

    def nxt(self):
        self.s = (self.s * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.s >> 16


def text_pixels(w, h, seed=12345):
    """0 = black, 255 = white; matches lossy_mono_text_pixels in
    tests/test_reader.c (walk-edge strokes)."""
    px = bytearray([255] * (w * h))
    r = Lcg(seed)
    y0 = 6
    while y0 + 12 < h:
        x0 = 6
        while x0 + 24 < w:
            ww = 14 + r.nxt() % 8
            for s in range(3):
                e0 = x0 + s * (ww // 3)
                e1 = e0 + 2 + r.nxt() % 3
                for yy in range(y0, y0 + 10):
                    e0 += (r.nxt() % 3) - 1
                    e1 += (r.nxt() % 3) - 1
                    e0 = max(e0, x0)
                    e1 = max(e1, e0 + 1)
                    e1 = min(e1, x0 + ww)
                    for xx in range(e0, min(e1, w)):
                        px[yy * w + xx] = 0
            x0 += 26
        y0 += 15
    return bytes(px)


def pack_1bpp(px, w, h):
    """MSB-first packed rows, bit 1 = white (DeviceGray sample 1)."""
    stride = (w + 7) // 8
    out = bytearray(stride * h)
    for y in range(h):
        for x in range(w):
            if px[y * w + x] >= 128:
                out[y * stride + x // 8] |= 0x80 >> (x % 8)
    return bytes(out)


def g4_encode(px, w, h):
    """G4 strip bytes via Pillow/libtiff (canonical polarity)."""
    inverted = bytes(255 - b for b in px)
    img = Image.frombytes("L", (w, h), inverted).convert("1")
    buf = io.BytesIO()
    img.save(buf, format="TIFF", compression="group4", tiffinfo={278: h})
    data = buf.getvalue()
    bo = "<" if data[:2] == b"II" else ">"
    off = struct.unpack(bo + "I", data[4:8])[0]
    n = struct.unpack(bo + "H", data[off : off + 2])[0]
    tags = {}
    for i in range(n):
        e = data[off + 2 + 12 * i : off + 14 + 12 * i]
        tag = struct.unpack(bo + "H", e[:2])[0]
        tags[tag] = struct.unpack(bo + "I", e[8:12])[0]
    return data[tags[273] : tags[273] + tags[279]]


def make_pdf(img_stream, img_dict_extra, w, h, w_pt, h_pt):
    """One-page PDF drawing the sole 1-bpc image at w_pt x h_pt points."""
    content = b"q %.2f 0 0 %.2f 50 600 cm /Im1 Do Q" % (w_pt, h_pt)
    objs = []
    objs.append(b"<< /Type /Catalog /Pages 2 0 R >>")
    objs.append(b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    objs.append(
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /XObject << /Im1 4 0 R >> >> /Contents 5 0 R >>"
    )
    objs.append(
        b"<< /Type /XObject /Subtype /Image /Width %d /Height %d "
        b"/ColorSpace /DeviceGray /BitsPerComponent 1 %s /Length %d >>"
        b"\nstream\n%s\nendstream" % (w, h, img_dict_extra, len(img_stream), img_stream)
    )
    objs.append(
        b"<< /Length %d >>\nstream\n%s\nendstream" % (len(content), content)
    )

    out = bytearray(b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for i, body in enumerate(objs, start=1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % i + body + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for off in offsets[1:]:
        out += b"%010d 00000 n \n" % off
    out += (
        b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF"
        % (len(objs) + 1, xref)
    )
    return bytes(out)


def save(path, data):
    with open(path, "wb") as f:
        f.write(data)
    print("%-46s %7d bytes" % (os.path.relpath(path, OUT), len(data)))


def g4_pdf(w, h, w_pt, h_pt, seed=12345):
    px = text_pixels(w, h, seed)
    g4 = g4_encode(px, w, h)
    extra = (
        b"/Filter /CCITTFaxDecode /DecodeParms "
        b"<< /K -1 /Columns %d /Rows %d >>" % (w, h)
    )
    return make_pdf(g4, extra, w, h, w_pt, h_pt)


def main():
    # 1.5 x 1.5 inch placement (108 pt): 900 px = 600 dpi, 450 px = 300 dpi.
    save(os.path.join(OUT, "lossy_mono.pdf"), g4_pdf(900, 900, 108.0, 108.0))
    save(os.path.join(OUT, "lossy_mono_low.pdf"), g4_pdf(450, 450, 108.0, 108.0))

    px = text_pixels(450, 450)
    flate = zlib.compress(pack_1bpp(px, 450, 450), 9)
    save(
        os.path.join(OUT, "lossy_mono_flate.pdf"),
        make_pdf(flate, b"/Filter /FlateDecode", 450, 450, 108.0, 108.0),
    )

    # Fuzz seed: smallest image the lossy pass will decode (65536 px).
    os.makedirs(FUZZ, exist_ok=True)
    save(os.path.join(FUZZ, "ccitt_g4.pdf"), g4_pdf(256, 256, 18.0, 18.0))


if __name__ == "__main__":
    main()
