# Regenerate armor_predref.pdf: a one-page PDF whose content stream is
# /Filter [/FlateDecode] with /DecodeParms held as an INDIRECT REFERENCE
# to a PNG-Up predictor dict (<< /Predictor 12 /Columns 32 >>), deflated at
# zlib level 1 so `tspdf compress` would want to re-encode it.
#
# tspdf_stream_decode does not resolve indirect /DecodeParms, so the
# armor-strip path in the save code must refuse to strip this stream —
# otherwise it bakes predictor-coded bytes into the output as plain Flate
# (silent corruption). See the "indirect DecodeParms" tests in test_cli.sh.
# Run with:  python3 tests/data/gen_armor_predref.py
import io
import os
import zlib

MARKER = "ArmorPredRefCheck"
COLUMNS = 32


def pred_up(data, columns):
    # PNG Up predictor (filter byte 2 per row), padded to whole rows.
    if len(data) % columns:
        data = data + b"\x00" * (columns - len(data) % columns)
    out = bytearray()
    prev = b"\x00" * columns
    for off in range(0, len(data), columns):
        row = data[off:off + columns]
        out.append(2)
        out += bytes((row[i] - prev[i]) & 0xFF for i in range(columns))
        prev = row
    return bytes(out)


content = f"BT /F1 18 Tf 72 700 Td ({MARKER}) Tj ET ".encode()
# Filler so the re-encode would win on size against the level-1 deflate.
content += b"\n% " + b"lorem ipsum filler " * 200
encoded = zlib.compress(pred_up(content, COLUMNS), 1)

buf = io.BytesIO()


def w(s):
    buf.write(s.encode("latin-1") if isinstance(s, str) else s)


offs = {}
w("%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
offs[1] = buf.tell()
w("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")
offs[2] = buf.tell()
w("2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")
offs[3] = buf.tell()
w("3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
  "/Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>\nendobj\n")
offs[4] = buf.tell()
w(f"4 0 obj\n<< /Length {len(encoded)} /Filter [/FlateDecode] "
  "/DecodeParms 6 0 R >>\nstream\n")
w(encoded)
w("\nendstream\nendobj\n")
offs[5] = buf.tell()
w("5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n")
offs[6] = buf.tell()
w(f"6 0 obj\n<< /Predictor 12 /Columns {COLUMNS} >>\nendobj\n")
xref_off = buf.tell()
w("xref\n0 7\n0000000000 65535 f \n")
for n in range(1, 7):
    w(f"{offs[n]:010d} 00000 n \n")
w(f"trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n{xref_off}\n%%EOF\n")

out_path = os.path.join(os.path.dirname(__file__), "armor_predref.pdf")
with open(out_path, "wb") as f:
    f.write(buf.getvalue())
print(f"wrote {out_path} ({buf.tell()} bytes)")
