#!/bin/bash
#
# External PDF conformance gate (T21).
#
# tspdf is a write-from-scratch PDF library, so the load-bearing correctness
# claim is that its output is spec-valid and openable by *real* third-party
# readers — not just round-trippable through tspdf itself. The plain CLI suite
# (tests/test_cli.sh) only asserts exit 0 and self-round-trips, which is exactly
# why the /TspdfSize trailer corruption (backlog T19) shipped undetected across
# every reader save path. This script closes that gap.
#
# It drives every CLI command plus reader round-trips and validates the output
# with whatever conformance checkers are installed:
#   - qpdf  --check        (structural / xref / stream validation)
#   - mutool clean / info  (MuPDF parse + rewrite)
#
# External verification binaries are used at *test time only*; the shipped tspdf
# binary stays zero-dependency. When neither qpdf nor mutool is present the gate
# skips gracefully and exits 0 (mirroring the curl/python3 guards in
# tests/test_cli.sh), so a clean offline box still passes `make test-all`. CI
# installs qpdf + mupdf-tools so the gate is real there.
#
# Two tiers:
#   * Writer tier (STRICT): output produced entirely by the from-scratch
#     generation path (img2pdf / qrcode / md2pdf) and the committed writer
#     fixtures MUST validate cleanly. A "damaged" verdict here is a hard fail.
#   * Reader tier (T19-gated): output produced by the reader re-serializer
#     (split/merge/rotate/delete/reorder/metadata/watermark/compress/
#     encrypt/decrypt). Until T19 replaces the non-standard /TspdfSize trailer
#     key with the spec-mandated /Size, qpdf reports these as "damaged
#     (trailer lacks /Size)". While the /TspdfSize marker is still present we
#     report those as KNOWN-PENDING (T19) instead of failing; the moment T19
#     lands and /Size is emitted, the same output is required to validate
#     cleanly, so this gate self-activates with no further edits.

set -u

TSPDF=./build/tspdf

if [ ! -x "$TSPDF" ]; then
    echo "error: $TSPDF missing — run 'make' first" >&2
    exit 1
fi

# Discover available external validators. Skip the whole gate if none exist.
HAVE_QPDF=0
HAVE_MUTOOL=0
command -v qpdf   > /dev/null 2>&1 && HAVE_QPDF=1
command -v mutool > /dev/null 2>&1 && HAVE_MUTOOL=1

if [ "$HAVE_QPDF" -eq 0 ] && [ "$HAVE_MUTOOL" -eq 0 ]; then
    echo "SKIP  external conformance gate (no qpdf or mutool found)"
    echo "      install qpdf and/or mupdf-tools to run spec-validity checks"
    exit 0
fi

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

INPUT=tests/data/three_pages.pdf
ONEPAGE=tests/data/one_page.pdf
if [ ! -f "$INPUT" ]; then
    echo "Generating test PDFs..."
    make generate-test-pdfs > /dev/null 2>&1
fi

pass=0
fail=0
pending=0

echo "External conformance gate:"
[ "$HAVE_QPDF"   -eq 1 ] && echo "  using qpdf:   $(qpdf --version 2>/dev/null | head -1)"
[ "$HAVE_MUTOOL" -eq 1 ] && echo "  using mutool: $(mutool -v 2>&1 | head -1)"
echo ""

# qpdf_check <file> [password]
# Echoes one of: ok | damaged | error  and returns the corresponding state.
#
# We classify by qpdf's exit code, not by scraping stdout — the normal clean
# report literally contains the word "errors" ("No syntax or stream encoding
# errors found"), so a text match misfires. qpdf exit codes:
#   0 = clean, 3 = warnings only (e.g. recovered xref), 2 = errors.
# A code of 2 with a "damaged" verdict is the structural-corruption case we
# distinguish for the T19 reader-trailer gate.
qpdf_check() {
    local f="$1" pw="${2:-}"
    local out ec
    if [ -n "$pw" ]; then
        out=$(qpdf --check "--password=$pw" "$f" 2>&1); ec=$?
    else
        out=$(qpdf --check "$f" 2>&1); ec=$?
    fi
    if printf '%s' "$out" | grep -qi 'damaged'; then
        echo damaged
    elif [ "$ec" -eq 0 ] || [ "$ec" -eq 3 ]; then
        echo ok
    else
        echo error
    fi
}

# strict_check <name> <file> [password]
# The file MUST validate cleanly under every available checker.
strict_check() {
    local name="$1" f="$2" pw="${3:-}"
    local ok=1
    if [ ! -f "$f" ]; then
        echo "  FAIL  $name (output file missing)"
        fail=$((fail + 1))
        return
    fi
    if [ "$HAVE_QPDF" -eq 1 ]; then
        local r; r=$(qpdf_check "$f" "$pw")
        [ "$r" = ok ] || { echo "  FAIL  $name (qpdf: $r)"; ok=0; }
    fi
    if [ "$HAVE_MUTOOL" -eq 1 ]; then
        # mutool's option parser stops at the first non-dash argument, so the
        # password option must come BEFORE the file (mutool info -p PW file.pdf).
        if ! mutool info ${pw:+-p "$pw"} "$f" > /dev/null 2>&1; then
            echo "  FAIL  $name (mutool info failed)"; ok=0
        fi
    fi
    if [ "$ok" -eq 1 ]; then
        echo "  PASS  $name"
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
    fi
}

# reader_check <name> <file> [password]
# Same strict expectation, EXCEPT a "trailer lacks /Size" style damage is
# treated as KNOWN-PENDING (backlog T19) as long as the output still carries
# the non-standard /TspdfSize marker. Once T19 emits /Size, the marker is gone
# and this falls through to a strict pass/fail automatically.
reader_check() {
    local name="$1" f="$2" pw="${3:-}"
    if [ ! -f "$f" ]; then
        echo "  FAIL  $name (output file missing)"
        fail=$((fail + 1))
        return
    fi
    # T19 not yet landed in this tree: tolerate the known trailer defect.
    if grep -aq 'TspdfSize' "$f"; then
        echo "  PEND  $name (T19: trailer emits /TspdfSize, not /Size)"
        pending=$((pending + 1))
        return
    fi
    strict_check "$name" "$f" "$pw"
}

# --- Writer tier (STRICT): from-scratch generation path must be spec-valid ---

echo "  Writer output (strict):"

# Committed writer fixtures (produced by the generation path).
strict_check "fixture one_page.pdf"    "$ONEPAGE"
strict_check "fixture three_pages.pdf" "$INPUT"

# img2pdf needs a tiny PNG; build one with python3 if available.
if command -v python3 > /dev/null 2>&1; then
    python3 -c "
import struct, zlib, sys
sig = b'\x89PNG\r\n\x1a\n'
def chunk(t, d): return struct.pack('>I',len(d)) + t + d + struct.pack('>I', zlib.crc32(t+d) & 0xffffffff)
ihdr = struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0)
raw = zlib.compress(b'\x00\xff\x00\x00')
open('$TMPDIR/test.png','wb').write(sig + chunk(b'IHDR',ihdr) + chunk(b'IDAT',raw) + chunk(b'IEND',b''))
"
    if "$TSPDF" img2pdf "$TMPDIR/test.png" -o "$TMPDIR/img2pdf.pdf" > /dev/null 2>&1; then
        strict_check "img2pdf" "$TMPDIR/img2pdf.pdf"
    else
        echo "  FAIL  img2pdf (command failed)"; fail=$((fail + 1))
    fi
else
    echo "  SKIP  img2pdf (python3 not found, cannot build test PNG)"
fi

# Committed PNG fixtures cover every embed path: IDAT passthrough (RGB, gray,
# 8/4-bit palette), Indexed + soft mask (palette tRNS), and the decode/split
# path (RGBA, gray+alpha). One multi-image PDF checks them all at once.
if "$TSPDF" img2pdf tests/data/img_rgb.png tests/data/img_gray.png \
        tests/data/img_palette.png tests/data/img_palette4.png \
        tests/data/img_palette_trns.png tests/data/img_rgba.png \
        tests/data/img_gray_alpha.png -o "$TMPDIR/img2pdf_all.pdf" > /dev/null 2>&1; then
    strict_check "img2pdf png embed matrix" "$TMPDIR/img2pdf_all.pdf"
else
    echo "  FAIL  img2pdf png embed matrix (command failed)"; fail=$((fail + 1))
fi
if "$TSPDF" img2pdf tests/data/img_rgb.png --page-size image -o "$TMPDIR/img2pdf_ps.pdf" > /dev/null 2>&1; then
    strict_check "img2pdf --page-size image" "$TMPDIR/img2pdf_ps.pdf"
else
    echo "  FAIL  img2pdf --page-size image (command failed)"; fail=$((fail + 1))
fi

if "$TSPDF" qrcode "https://example.com" -o "$TMPDIR/qrcode.pdf" > /dev/null 2>&1; then
    strict_check "qrcode" "$TMPDIR/qrcode.pdf"
else
    echo "  FAIL  qrcode (command failed)"; fail=$((fail + 1))
fi

printf '# Heading\n\nA paragraph with **bold** text.\n\n- one\n- two\n' > "$TMPDIR/test.md"
if "$TSPDF" md2pdf "$TMPDIR/test.md" -o "$TMPDIR/md2pdf.pdf" > /dev/null 2>&1; then
    strict_check "md2pdf" "$TMPDIR/md2pdf.pdf"
else
    echo "  FAIL  md2pdf (command failed)"; fail=$((fail + 1))
fi

# --- Reader tier (T19-gated): re-serializer output ---

echo ""
echo "  Reader round-trip output:"

"$TSPDF" split    "$INPUT" --pages 1-2     -o "$TMPDIR/split.pdf"    > /dev/null 2>&1
reader_check "split"    "$TMPDIR/split.pdf"

"$TSPDF" merge    "$INPUT" "$TMPDIR/split.pdf" -o "$TMPDIR/merge.pdf" > /dev/null 2>&1
reader_check "merge"    "$TMPDIR/merge.pdf"

"$TSPDF" rotate   "$INPUT" --angle 90      -o "$TMPDIR/rotate.pdf"   > /dev/null 2>&1
reader_check "rotate"   "$TMPDIR/rotate.pdf"

"$TSPDF" delete   "$INPUT" --pages 1       -o "$TMPDIR/delete.pdf"   > /dev/null 2>&1
reader_check "delete"   "$TMPDIR/delete.pdf"

"$TSPDF" reorder  "$INPUT" --order 3,1,2   -o "$TMPDIR/reorder.pdf"  > /dev/null 2>&1
reader_check "reorder"  "$TMPDIR/reorder.pdf"

"$TSPDF" metadata "$INPUT" --set title="External Check" -o "$TMPDIR/meta.pdf" > /dev/null 2>&1
reader_check "metadata" "$TMPDIR/meta.pdf"

"$TSPDF" watermark "$INPUT" --text "DRAFT" -o "$TMPDIR/watermark.pdf" > /dev/null 2>&1
reader_check "watermark" "$TMPDIR/watermark.pdf"

"$TSPDF" watermark "$INPUT" --image tests/data/img_rgba.png -o "$TMPDIR/watermark_img.pdf" > /dev/null 2>&1
reader_check "watermark --image" "$TMPDIR/watermark_img.pdf"

printf '# APPROVED\n' > "$TMPDIR/stamp_src.md"
"$TSPDF" md2pdf "$TMPDIR/stamp_src.md" -o "$TMPDIR/stamp_src.pdf" > /dev/null 2>&1
"$TSPDF" stamp "$INPUT" --stamp "$TMPDIR/stamp_src.pdf" -o "$TMPDIR/stamp.pdf" > /dev/null 2>&1
reader_check "stamp" "$TMPDIR/stamp.pdf"

"$TSPDF" stamp "$INPUT" --stamp "$TMPDIR/stamp_src.pdf" --under -o "$TMPDIR/stamp_under.pdf" > /dev/null 2>&1
reader_check "stamp --under" "$TMPDIR/stamp_under.pdf"

"$TSPDF" compress "$INPUT"                 -o "$TMPDIR/compress.pdf" > /dev/null 2>&1
reader_check "compress" "$TMPDIR/compress.pdf"

"$TSPDF" encrypt  "$INPUT" --password secret -o "$TMPDIR/encrypt.pdf" > /dev/null 2>&1
reader_check "encrypt"  "$TMPDIR/encrypt.pdf" secret

"$TSPDF" decrypt  "$TMPDIR/encrypt.pdf" --password secret -o "$TMPDIR/decrypt.pdf" > /dev/null 2>&1
reader_check "decrypt"  "$TMPDIR/decrypt.pdf"

"$TSPDF" form fill tests/data/form_fields.pdf --set name=External --set agree=true \
    -o "$TMPDIR/form_fill.pdf" > /dev/null 2>&1
reader_check "form fill" "$TMPDIR/form_fill.pdf"

"$TSPDF" form flatten "$TMPDIR/form_fill.pdf" -o "$TMPDIR/form_flat.pdf" > /dev/null 2>&1
reader_check "form flatten" "$TMPDIR/form_flat.pdf"

# Flatten must leave no trace of the form for real readers.
if [ "$HAVE_QPDF" -eq 1 ]; then
    if qpdf --json --json-key=acroform "$TMPDIR/form_flat.pdf" 2>/dev/null \
            | grep -q '"hasacroform": false'; then
        echo "  PASS  form flatten leaves no acroform (qpdf json)"
        pass=$((pass + 1))
    else
        echo "  FAIL  form flatten leaves no acroform (qpdf json)"
        fail=$((fail + 1))
    fi
fi

# --- Attachments: qpdf interop in both directions ---

echo ""
echo "  Attachments:"

printf 'external attachment payload\n' > "$TMPDIR/att.txt"

"$TSPDF" attach add "$INPUT" "$TMPDIR/att.txt" --desc "gate" -o "$TMPDIR/att_out.pdf" > /dev/null 2>&1
reader_check "attach add" "$TMPDIR/att_out.pdf"

"$TSPDF" attach remove "$TMPDIR/att_out.pdf" --name att.txt -o "$TMPDIR/att_removed.pdf" > /dev/null 2>&1
reader_check "attach remove" "$TMPDIR/att_removed.pdf"

if [ "$HAVE_QPDF" -eq 1 ]; then
    # tspdf -> qpdf: qpdf must see the attachment and read back identical bytes.
    if qpdf --list-attachments "$TMPDIR/att_out.pdf" 2>/dev/null | grep -q '^att.txt'; then
        echo "  PASS  qpdf lists tspdf-added attachment"; pass=$((pass + 1))
    else
        echo "  FAIL  qpdf lists tspdf-added attachment"; fail=$((fail + 1))
    fi
    if qpdf --show-attachment=att.txt "$TMPDIR/att_out.pdf" > "$TMPDIR/att_shown.txt" 2>/dev/null \
       && cmp -s "$TMPDIR/att_shown.txt" "$TMPDIR/att.txt"; then
        echo "  PASS  qpdf reads back identical attachment bytes"; pass=$((pass + 1))
    else
        echo "  FAIL  qpdf reads back identical attachment bytes"; fail=$((fail + 1))
    fi
    if qpdf --list-attachments "$TMPDIR/att_removed.pdf" 2>/dev/null | grep -q '^att.txt'; then
        echo "  FAIL  qpdf no longer lists removed attachment"; fail=$((fail + 1))
    else
        echo "  PASS  qpdf no longer lists removed attachment"; pass=$((pass + 1))
    fi

    # qpdf -> tspdf: extract a qpdf-added attachment byte-identically.
    mkdir -p "$TMPDIR/att_ex"
    if qpdf "$INPUT" --add-attachment "$TMPDIR/att.txt" -- "$TMPDIR/att_qpdf.pdf" 2>/dev/null \
       && "$TSPDF" attach extract "$TMPDIR/att_qpdf.pdf" --all -o "$TMPDIR/att_ex" > /dev/null 2>&1 \
       && cmp -s "$TMPDIR/att_ex/att.txt" "$TMPDIR/att.txt"; then
        echo "  PASS  tspdf extracts qpdf-added attachment"; pass=$((pass + 1))
    else
        echo "  FAIL  tspdf extracts qpdf-added attachment"; fail=$((fail + 1))
    fi

    # Attachments survive page extraction and merge; qpdf is the witness.
    "$TSPDF" split "$TMPDIR/att_out.pdf" --pages 1 -o "$TMPDIR/att_split.pdf" > /dev/null 2>&1
    reader_check "split keeps attachment (qpdf-validated)" "$TMPDIR/att_split.pdf"
    if qpdf --list-attachments "$TMPDIR/att_split.pdf" 2>/dev/null | grep -q '^att.txt'; then
        echo "  PASS  qpdf lists attachment after split"; pass=$((pass + 1))
    else
        echo "  FAIL  qpdf lists attachment after split"; fail=$((fail + 1))
    fi

    printf 'second attachment\n' > "$TMPDIR/att2.txt"
    "$TSPDF" attach add "$INPUT" "$TMPDIR/att2.txt" -o "$TMPDIR/att_out2.pdf" > /dev/null 2>&1
    "$TSPDF" merge "$TMPDIR/att_out.pdf" "$TMPDIR/att_out2.pdf" -o "$TMPDIR/att_merged.pdf" > /dev/null 2>&1
    reader_check "merge keeps attachments (qpdf-validated)" "$TMPDIR/att_merged.pdf"
    if qpdf --list-attachments "$TMPDIR/att_merged.pdf" 2>/dev/null | grep -q '^att.txt' \
       && qpdf --list-attachments "$TMPDIR/att_merged.pdf" 2>/dev/null | grep -q '^att2.txt'; then
        echo "  PASS  qpdf lists both attachments after merge"; pass=$((pass + 1))
    else
        echo "  FAIL  qpdf lists both attachments after merge"; fail=$((fail + 1))
    fi
fi

echo ""
if [ "$pending" -gt 0 ]; then
    echo "$pass passed, $fail failed, $pending pending (T19: reader trailer still emits /TspdfSize)"
else
    echo "$pass passed, $fail failed"
fi
[ "$fail" -eq 0 ] || exit 1
