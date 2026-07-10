# Regenerates tests/data/indirect_resources.pdf: a 2-page PDF where BOTH
# pages point their /Resources at ONE shared indirect dict (obj 7), whose
# /Font is ITSELF an indirect dict (obj 8). This is the shape produced by
# pymupdf-saved and pdfTeX/arXiv files. It is the regression fixture for the
# overlay-resource-merge fix: merging watermark/pagenum resources used to
# append a duplicate /Resources key to the page dict, orphaning the original
# fonts (pages rendered near-blank in poppler).
#
# Run from the repo root:
#   python3 tests/data/gen_indirect_resources.py

content1 = b"BT /F1 24 Tf 72 700 Td (Hello page one) Tj ET"
content2 = b"BT /F1 24 Tf 72 700 Td (Hello page two) Tj ET"

objs = {
    1: b"<< /Type /Catalog /Pages 2 0 R >>",
    2: b"<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
    3: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
       b"/Resources 7 0 R /Contents 5 0 R >>",
    4: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
       b"/Resources 7 0 R /Contents 6 0 R >>",
    5: b"<< /Length %d >>\nstream\n%s\nendstream" % (len(content1), content1),
    6: b"<< /Length %d >>\nstream\n%s\nendstream" % (len(content2), content2),
    7: b"<< /Font 8 0 R >>",
    8: b"<< /F1 9 0 R >>",
    9: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
}

buf = b"%PDF-1.4\n"
offs = {}
for n in sorted(objs):
    offs[n] = len(buf)
    buf += b"%d 0 obj\n" % n + objs[n] + b"\nendobj\n"
xp = len(buf)
mx = max(objs)
buf += b"xref\n0 %d\n0000000000 65535 f \n" % (mx + 1)
for n in range(1, mx + 1):
    buf += b"%010d 00000 n \n" % offs[n]
buf += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (mx + 1, xp)

with open("tests/data/indirect_resources.pdf", "wb") as f:
    f.write(buf)
print("wrote tests/data/indirect_resources.pdf (%d bytes)" % len(buf))
