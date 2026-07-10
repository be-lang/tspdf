# Regenerates the `compress --lossy` fixtures:
#
#   lossy_scan.pdf        2 pages: a 600-dpi-equivalent FlateDecode RGB
#                         "scan" (360x480 px drawn at 43.2x57.6 pt) plus a
#                         plain text page. --lossy must shrink it a lot and
#                         leave the text page's content alone.
#   lossy_scan_dct.pdf    the same image already stored as a baseline JPEG
#                         (DCTDecode) at 600 dpi: must be downsampled.
#   lossy_progressive.pdf the image as a PROGRESSIVE JPEG: the from-scratch
#                         decoder rejects it, so it must pass through
#                         byte-identical.
#   lossy_smask.pdf       a flate image carrying an /SMask: must pass
#                         through untouched.
#   lossy_lowdpi.pdf      a flate image already at ~100 dpi: must pass
#                         through untouched.
#
# Deterministic (fixed LCG seed); images are noisy on purpose — a clean
# gradient deflates so well that keeping the original is the right call and
# the lossy path would (correctly) do nothing.
#
# Usage:
#   uv run --python 3.12 --with pillow python tests/data/gen_lossy_fixtures.py
# (Pillow is only needed for the two JPEG variants; the flate fixtures are
# regenerated even without it.)

import os
import zlib

OUT = os.path.dirname(os.path.abspath(__file__))


def pixels_rgb(w, h):
    """Colored scan-ish noise: gradient + LCG noise, channels offset by a
    constant 20 so gray detection must NOT trigger."""
    buf = bytearray(w * h * 3)
    lcg = 42
    i = 0
    for y in range(h):
        for x in range(w):
            lcg = (lcg * 1664525 + 1013904223) & 0xFFFFFFFF
            base = (x * 199 // w + y * 37 // h + ((lcg >> 24) & 7)) & 0xFF
            buf[i] = min(base + 20, 255)
            buf[i + 1] = base
            buf[i + 2] = max(base - 20, 0)
            i += 3
    return bytes(buf)


def pixels_gray(w, h, noise_bits=2):
    buf = bytearray(w * h)
    lcg = 7
    i = 0
    mask = (1 << noise_bits) - 1
    for y in range(h):
        for x in range(w):
            lcg = (lcg * 1664525 + 1013904223) & 0xFFFFFFFF
            buf[i] = (x * 211 // w + y * 29 // h + ((lcg >> 24) & mask)) & 0xFF
            i += 1
    return bytes(buf)


def build_pdf(objects):
    """objects: list of bytes bodies for objects 1..n (without 'N 0 obj')."""
    out = bytearray(b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for i, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % i + body + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objects) + 1)
    for off in offsets[1:]:
        out += b"%010d 00000 n \n" % off
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (
        len(objects) + 1, xref)
    return bytes(out)


def image_obj(data, w, h, cs, filt, extra=b""):
    return (b"<< /Type /XObject /Subtype /Image /Width %d /Height %d "
            b"/ColorSpace /%s /BitsPerComponent 8 /Filter /%s %s/Length %d >>\n"
            b"stream\n" % (w, h, cs, filt, extra, len(data))) + data + b"\nendstream"


def content_obj(text):
    return b"<< /Length %d >>\nstream\n%s\nendstream" % (len(text), text)


def scan_pdf(img_body, w_pt, h_pt, extra_objs=()):
    """One image page + one text page. Image object is 4, its content 5;
    the text page uses font 7 / content 8. extra_objs go after that."""
    draw = b"q %.1f 0 0 %.1f 100 300 cm /Im1 Do Q" % (w_pt, h_pt)
    text = (b"BT /F1 14 Tf 72 700 Td (Lossy fixture text page) Tj ET")
    objs = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /XObject << /Im1 4 0 R >> >> /Contents 5 0 R >>",
        img_body,
        content_obj(draw),
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /Font << /F1 7 0 R >> >> /Contents 8 0 R >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        content_obj(text),
    ]
    objs.extend(extra_objs)
    return build_pdf(objs)


def write(name, data):
    path = os.path.join(OUT, name)
    with open(path, "wb") as f:
        f.write(data)
    print("%s: %d bytes" % (name, len(data)))


def main():
    # 360x480 px drawn at 43.2x57.6 pt = exactly 600 dpi.
    W, H = 360, 480
    W_PT, H_PT = 43.2, 57.6
    rgb = pixels_rgb(W, H)

    flate = zlib.compress(rgb, 9)
    write("lossy_scan.pdf",
          scan_pdf(image_obj(flate, W, H, b"DeviceRGB", b"FlateDecode"),
                   W_PT, H_PT))

    # Already at ~100 dpi (300x400 px over 3x4 inches): must stay untouched.
    g = pixels_gray(300, 400)
    write("lossy_lowdpi.pdf",
          scan_pdf(image_obj(zlib.compress(g, 9), 300, 400, b"DeviceGray",
                             b"FlateDecode"), 216.0, 288.0))

    # High-dpi but with an /SMask (object 9): must stay untouched.
    base = pixels_gray(300, 300)
    mask = pixels_gray(300, 300, noise_bits=1)
    write("lossy_smask.pdf",
          scan_pdf(image_obj(zlib.compress(base, 9), 300, 300, b"DeviceGray",
                             b"FlateDecode", extra=b"/SMask 9 0 R "),
                   36.0, 36.0,
                   extra_objs=[image_obj(zlib.compress(mask, 9), 300, 300,
                                         b"DeviceGray", b"FlateDecode")]))

    # The JPEG variants need Pillow.
    try:
        from PIL import Image
    except ImportError:
        print("warning: Pillow not available, JPEG fixtures not regenerated")
        return
    import io

    img = Image.frombytes("RGB", (W, H), rgb)

    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=90)  # baseline
    write("lossy_scan_dct.pdf",
          scan_pdf(image_obj(buf.getvalue(), W, H, b"DeviceRGB", b"DCTDecode"),
                   W_PT, H_PT))

    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=90, progressive=True)
    write("lossy_progressive.pdf",
          scan_pdf(image_obj(buf.getvalue(), W, H, b"DeviceRGB", b"DCTDecode"),
                   W_PT, H_PT))


if __name__ == "__main__":
    main()
