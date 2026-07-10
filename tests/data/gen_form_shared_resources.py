# Regenerates tests/data/form_shared_resources.pdf: a 2-page AcroForm where
# BOTH pages point their /Resources at ONE shared indirect dict (obj 9), which
# carries a /Font (Helv). Each page has one text field with a distinct value.
# Written raw (not via pymupdf) so the /Resources sharing is byte-exact; it is
# the regression fixture for the flatten-shared-/Resources fix (finding M3).
#
# Run from the repo root:
#   python3 tests/data/gen_form_shared_resources.py

objs = {
    1: b"<< /Type /Catalog /Pages 2 0 R /AcroForm 5 0 R >>",
    2: b"<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
    3: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] "
       b"/Resources 9 0 R /Contents 8 0 R /Annots [6 0 R] >>",
    4: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] "
       b"/Resources 9 0 R /Contents 8 0 R /Annots [7 0 R] >>",
    5: b"<< /Fields [6 0 R 7 0 R] /DR << /Font << /Helv 10 0 R >> >> "
       b"/DA (/Helv 0 Tf 0 g) >>",
    6: b"<< /Type /Annot /Subtype /Widget /FT /Tx /T (f1) /V (Alpha) "
       b"/Rect [10 10 200 30] /P 3 0 R >>",
    7: b"<< /Type /Annot /Subtype /Widget /FT /Tx /T (f2) /V (Beta) "
       b"/Rect [10 10 200 30] /P 4 0 R >>",
    8: b"<< /Length 0 >>\nstream\n\nendstream",
    9: b"<< /Font << /Helv 10 0 R >> >>",  # shared by pages 3 and 4
    10: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
        b"/Encoding /WinAnsiEncoding >>",
}

out = b"%PDF-1.5\n"
offs = {}
for n in sorted(objs):
    offs[n] = len(out)
    out += b"%d 0 obj\n" % n + objs[n] + b"\nendobj\n"
xref_pos = len(out)
maxn = max(objs)
out += b"xref\n0 %d\n0000000000 65535 f \n" % (maxn + 1)
for n in range(1, maxn + 1):
    out += b"%010d 00000 n \n" % offs[n]
out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF" % (
    maxn + 1, xref_pos)

with open("tests/data/form_shared_resources.pdf", "wb") as f:
    f.write(out)
print("wrote tests/data/form_shared_resources.pdf", len(out))
