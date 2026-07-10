# Regenerates the CCITT codec oracle fixtures (tests/test_main.c, "CCITT
# codec" section). Two independent known-good encoders provide the coded
# streams our decoder is held to:
#
#   Pillow/libtiff  ccitt_runs_white.g3   G3 1-D, every white terminating +
#                   ccitt_runs_black.g3   makeup code (and black resp.)
#                   ccitt_text.g4         G4/MMR of a text-like bitmap
#   Ghostscript     ccitt_text_k0.bin         K=0, no EOLs
#   (CCITTFaxEncode ccitt_text_k0_align.bin   K=0, EncodedByteAlign
#   filter)         ccitt_text_k2_eol.bin     K=2 mixed 2-D, EOLs + tag bits
#                   ccitt_text_k4_noeol.bin   K=4 mixed 2-D, no EOLs (1-D
#                                             line cadence, no tag bits)
#                   ccitt_text_g4_align.bin   K=-1, EncodedByteAlign
#                   ccitt_text_g4_black1.bin  K=-1, BlackIs1 true
#
# Source bitmaps are committed as PBM (P4): ccitt_runs.pbm is shared by the
# two run fixtures' geometry; ccitt_text.pbm is the text-like page. In PBM,
# a 1 bit is BLACK; in the raw data handed to gs/libtiff with the default
# BlackIs1=false convention a 0 bit is black — the script inverts
# accordingly. Everything is deterministic (fixed LCG, no time()).
#
# Usage (gs must be on PATH; run from the repo root):
#   uv run --python 3.12 --with pillow python tests/data/gen_ccitt_fixtures.py

import io
import os
import struct
import subprocess

from PIL import Image

OUT = os.path.dirname(os.path.abspath(__file__))

# --- Deterministic bitmaps ---


def lcg(seed):
    state = seed

    def nxt():
        nonlocal state
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        return state >> 16

    return nxt


def runs_bitmap(black):
    """Rows exercising every terminating (0..63) and makeup (64..2560) code
    of one color, plus a few makeup+term combinations. Width 2704 so the
    2560 makeup + long terminator fits (and is divisible by 8)."""
    W = 2704
    lengths = list(range(64)) + list(range(64, 2561, 64)) + [
        2560 + 63, 1728 + 63, 1792 + 1, 63 + 64,
    ]
    rows = []
    for L in lengths:
        row = bytearray([255] * W)  # 255 = white
        if black:
            # [white 1][black L][white fill]
            for x in range(1, 1 + L):
                row[x] = 0
            if L == 0:
                continue  # encoders never emit zero-length mid-line runs
        else:
            # [white L][black 2][white fill]
            row[L] = 0
            row[L + 1] = 0
            for x in range(L):
                row[x] = 255
        rows.append(bytes(row))
    return W, len(rows), b"".join(rows)


def text_bitmap():
    """A 400x120 text-like page: rows of word-ish black blobs with jitter,
    exercising vertical/pass/horizontal G4 modes densely."""
    W, H = 400, 120
    px = bytearray([255] * (W * H))
    rnd = lcg(0xC0FFEE)
    y = 6
    while y + 8 < H:
        x = 4
        while x + 10 < W:
            wordw = 8 + rnd() % 30
            if x + wordw >= W - 4:
                break
            for yy in range(y, y + 6):
                for xx in range(x, x + wordw):
                    # strokes: vertical bars + a baseline, with noise
                    bar = (xx - x) % 5 < 2 or yy == y + 5
                    if bar and rnd() % 8 != 0:
                        px[yy * W + xx] = 0
            x += wordw + 3 + rnd() % 6
        y += 9
    return W, H, bytes(px)


def pack_bits(w, h, pixels, one_is_black):
    """Pack byte pixels (0 = black) into 1-bpp rows, MSB first."""
    stride = (w + 7) // 8
    out = bytearray(stride * h)
    for yy in range(h):
        for xx in range(w):
            black = pixels[yy * w + xx] < 128
            bit = 1 if (black == one_is_black) else 0
            if bit:
                out[yy * stride + xx // 8] |= 0x80 >> (xx % 8)
    return bytes(out)


def write_pbm(path, w, h, pixels):
    with open(path, "wb") as f:
        f.write(b"P4\n%d %d\n" % (w, h))
        f.write(pack_bits(w, h, pixels, one_is_black=True))


# --- Encoders ---


def tiff_strip(img_bytes):
    """Return the single raw strip of a TIFF produced by Pillow/libtiff."""
    data = img_bytes
    bo = "<" if data[:2] == b"II" else ">"
    off = struct.unpack(bo + "I", data[4:8])[0]
    n = struct.unpack(bo + "H", data[off : off + 2])[0]
    tags = {}
    for i in range(n):
        e = data[off + 2 + 12 * i : off + 14 + 12 * i]
        tag, _typ, _cnt = struct.unpack(bo + "HHI", e[:8])
        val = struct.unpack(bo + "I", e[8:12])[0]
        tags[tag] = val
    assert tags.get(278, 0) >= 1, "rows per strip missing"
    return data[tags[273] : tags[273] + tags[279]]


def pil_encode(w, h, pixels, compression):
    # Pillow writes mode-"1" CCITT TIFFs with PhotometricInterpretation =
    # MinIsBlack and codes the bits as-is, which flips the fax polarity
    # (coded "white" = visually black). Invert on the way in so the strip
    # codes the intended image in the canonical convention.
    inverted = bytes(255 - b for b in pixels)
    img = Image.frombytes("L", (w, h), inverted).convert("1")
    buf = io.BytesIO()
    # tiffinfo 278 = RowsPerStrip: force a single strip so the strip bytes
    # are one continuous CCITT stream.
    img.save(buf, format="TIFF", compression=compression, tiffinfo={278: h})
    return tiff_strip(buf.getvalue())


def gs_encode(w, h, pixels, params):
    raw = pack_bits(w, h, pixels, one_is_black=False)  # 0 bit = black
    tmp_in = os.path.join(OUT, "_gs_in.bin")
    tmp_out = os.path.join(OUT, "_gs_out.bin")
    with open(tmp_in, "wb") as f:
        f.write(raw)
    ps = (
        "/inf (%s) (r) file def "
        "/outf (%s) (w) file def "
        "/enc outf << /Columns %d /Rows %d %s >> /CCITTFaxEncode filter def "
        "/buf 4096 string def "
        "{ inf buf readstring exch enc exch writestring not { exit } if } loop "
        "enc closefile outf closefile quit" % (tmp_in, tmp_out, w, h, params)
    )
    subprocess.run(
        ["gs", "-q", "-dNODISPLAY", "-dBATCH", "-dNOSAFER", "-c", ps],
        check=True,
    )
    with open(tmp_out, "rb") as f:
        out = f.read()
    os.remove(tmp_in)
    os.remove(tmp_out)
    return out


def save(name, data):
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(data)
    print("%-28s %6d bytes" % (name, len(data)))


def main():
    for black in (False, True):
        w, h, px = runs_bitmap(black)
        name = "ccitt_runs_black" if black else "ccitt_runs_white"
        write_pbm(os.path.join(OUT, name + ".pbm"), w, h, px)
        save(name + ".g3", pil_encode(w, h, px, "group3"))
        print("%-28s %dx%d" % (name + ".pbm", w, h))

    w, h, px = text_bitmap()
    write_pbm(os.path.join(OUT, "ccitt_text.pbm"), w, h, px)
    print("%-28s %dx%d" % ("ccitt_text.pbm", w, h))
    save("ccitt_text.g4", pil_encode(w, h, px, "group4"))
    save("ccitt_text_k0.bin", gs_encode(w, h, px, "/K 0 /EndOfLine false"))
    save("ccitt_text_k0_align.bin",
         gs_encode(w, h, px, "/K 0 /EndOfLine false /EncodedByteAlign true"))
    save("ccitt_text_k2_eol.bin", gs_encode(w, h, px, "/K 2 /EndOfLine true"))
    save("ccitt_text_k4_noeol.bin", gs_encode(w, h, px, "/K 4 /EndOfLine false"))
    save("ccitt_text_g4_align.bin",
         gs_encode(w, h, px, "/K -1 /EncodedByteAlign true"))
    save("ccitt_text_g4_black1.bin", gs_encode(w, h, px, "/K -1 /BlackIs1 true"))


if __name__ == "__main__":
    main()
