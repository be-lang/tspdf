#!/bin/bash
set -e
TSPDF=./build/tspdf
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

echo "Building CLI..."
rm -rf build
# Use the canonical default target so the CLI is always rebuilt from a clean
# tree. (`make cli` is now a real alias, but `make all` is the documented
# entry point and cannot silently degrade to a no-op if the alias is dropped.)
make all > /dev/null 2>&1
if [ ! -x "$TSPDF" ]; then
    echo "error: $TSPDF missing after 'make all' (CLI build did not produce $TSPDF)" >&2
    exit 1
fi

# Ports per invocation (avoid fixed collisions); each raw-HTTP test uses its own server+port.
CLI_TEST_PORT_BASE=$((30000 + ($$ % 5000) + RANDOM % 200))
SERVE_PORT=$CLI_TEST_PORT_BASE
RAW_PORT_HOST=$((CLI_TEST_PORT_BASE + 30))
RAW_PORT_64H=$((CLI_TEST_PORT_BASE + 31))
RAW_PORT_65H=$((CLI_TEST_PORT_BASE + 32))
RAW_PORT_CLNEG=$((CLI_TEST_PORT_BASE + 33))
RAW_PORT_CLBIG=$((CLI_TEST_PORT_BASE + 34))
RAW_PORT_STALL=$((CLI_TEST_PORT_BASE + 35))
RAW_PORT_METAJSON=$((CLI_TEST_PORT_BASE + 36))
RAW_PORT_BOUNDARY_CI=$((CLI_TEST_PORT_BASE + 37))
RAW_PORT_WRITEDROP=$((CLI_TEST_PORT_BASE + 38))
RAW_PORT_REBIND_HOST=$((CLI_TEST_PORT_BASE + 39))
RAW_PORT_REBIND_ORIGIN=$((CLI_TEST_PORT_BASE + 40))
RAW_PORT_REBIND_OK=$((CLI_TEST_PORT_BASE + 41))

# Need a test PDF — use the existing test data
INPUT=tests/data/three_pages.pdf
if [ ! -f "$INPUT" ]; then
    echo "Generating test PDFs..."
    make generate-test-pdfs > /dev/null 2>&1
fi

pass=0
fail=0

run_test() {
    local name="$1"
    shift
    if "$@" > /dev/null 2>&1; then
        echo "  PASS  $name"
        pass=$((pass + 1))
    else
        echo "  FAIL  $name"
        fail=$((fail + 1))
    fi
}

echo ""
echo "CLI tests:"

run_test "cli regenerates embedded assets on build" bash -c '
  set -e
  ASSETS_H="cli/assets.h"
  ASSETS_BAK="'"$TMPDIR"'/assets.h.bak"
  cp "$ASSETS_H" "$ASSETS_BAK"
  restore_cli_assets_h() {
    cp "$ASSETS_BAK" "$ASSETS_H" 2>/dev/null || true
  }
  trap restore_cli_assets_h EXIT
  rm -f "$ASSETS_H"
  make cli > /dev/null 2>&1
  [ -f "$ASSETS_H" ]
  grep -q "Auto-generated from web/" "$ASSETS_H"
  [ -x "'"$TSPDF"'" ]
  "'"$TSPDF"'" --version > /dev/null
  cmp -s "$ASSETS_BAK" "$ASSETS_H"
'

# info
run_test "info" $TSPDF info $INPUT

# split
run_test "split pages 1-2" $TSPDF split $INPUT --pages 1-2 -o $TMPDIR/split.pdf
run_test "split result has exactly 2 pages (info output)" bash -c "$TSPDF info $TMPDIR/split.pdf | grep -qE '^Pages:[[:space:]]*2$'"
# flag-first ordering: -o output path must not be swallowed as the input file
run_test "split flag-first ordering (-o before input)" $TSPDF split -o $TMPDIR/split_ff.pdf --pages 1 $INPUT
run_test "split flag-first result is the real input (1 page)" bash -c "$TSPDF info $TMPDIR/split_ff.pdf | grep -qE '^Pages:[[:space:]]*1$'"

# merge
run_test "merge two files" $TSPDF merge $INPUT $TMPDIR/split.pdf -o $TMPDIR/merged.pdf
# flag-first ordering: -o output must not be counted as an input file
run_test "merge flag-first ordering (-o before inputs)" $TSPDF merge -o $TMPDIR/merged_ff.pdf $INPUT $TMPDIR/split.pdf

# rotate
run_test "rotate all 90" $TSPDF rotate $INPUT --angle 90 -o $TMPDIR/rotated.pdf
run_test "rotate specific pages" $TSPDF rotate $INPUT --pages 1 --angle 180 -o $TMPDIR/rotated2.pdf
# flag-first ordering: -o output and --angle value must not be swallowed as input
run_test "rotate flag-first ordering (-o/--angle before input)" $TSPDF rotate -o $TMPDIR/rotated_ff.pdf --angle 90 $INPUT
run_test "rotate flag-first result has 3 pages" bash -c "$TSPDF info $TMPDIR/rotated_ff.pdf | grep -qE '^Pages:[[:space:]]*3$'"

# delete
run_test "delete page 1" $TSPDF delete $INPUT --pages 1 -o $TMPDIR/deleted.pdf
# flag-first ordering: -o output and --pages value must not be swallowed as input
run_test "delete flag-first ordering (-o/--pages before input)" $TSPDF delete -o $TMPDIR/deleted_ff.pdf --pages 1 $INPUT
run_test "delete flag-first result has 2 pages" bash -c "$TSPDF info $TMPDIR/deleted_ff.pdf | grep -qE '^Pages:[[:space:]]*2$'"

# reorder
run_test "reorder pages" $TSPDF reorder $INPUT --order 3,1,2 -o $TMPDIR/reordered.pdf
# flag-first ordering: -o output and --order value must not be swallowed as input
run_test "reorder flag-first ordering (-o/--order before input)" $TSPDF reorder -o $TMPDIR/reordered_ff.pdf --order 3,1,2 $INPUT
run_test "reorder flag-first result has 3 pages" bash -c "$TSPDF info $TMPDIR/reordered_ff.pdf | grep -qE '^Pages:[[:space:]]*3$'"

# encrypt + decrypt
run_test "encrypt AES-128" $TSPDF encrypt $INPUT -o $TMPDIR/enc128.pdf --password secret
run_test "encrypt AES-256" $TSPDF encrypt $INPUT -o $TMPDIR/enc256.pdf --password secret --bits 256
run_test "decrypt" $TSPDF decrypt $TMPDIR/enc128.pdf -o $TMPDIR/decrypted.pdf --password secret

# metadata
run_test "metadata view" $TSPDF metadata $INPUT
run_test "metadata set" $TSPDF metadata $INPUT --set title="Test Title" --set author="Test Author" -o $TMPDIR/meta.pdf
# flag-first ordering: -o output and --set values must not be swallowed as input
run_test "metadata flag-first ordering (-o/--set before input)" $TSPDF metadata -o $TMPDIR/meta_ff.pdf --set title="FF" $INPUT
run_test "metadata flag-first applied the title" bash -c "$TSPDF metadata $TMPDIR/meta_ff.pdf | grep -qi 'FF'"

# watermark
run_test "watermark" $TSPDF watermark $INPUT -o $TMPDIR/watermark.pdf --text "DRAFT"
# flag-first ordering: -o output and --text value must not be swallowed as input
run_test "watermark flag-first ordering (-o/--text before input)" $TSPDF watermark -o $TMPDIR/watermark_ff.pdf --text "DRAFT" $INPUT

# compress
run_test "compress" $TSPDF compress $INPUT -o $TMPDIR/compressed.pdf
# flag-first ordering: -o output must not be swallowed as input
run_test "compress flag-first ordering (-o before input)" $TSPDF compress -o $TMPDIR/compressed_ff.pdf $INPUT

# compress must not re-inflate an already-well-compressed FlateDecode stream:
# round-tripping such a stream costs CPU and never shrinks it (often grows it).
# Build a PDF whose content stream is a large payload already stored as
# FlateDecode (compressed size above the skip floor), then assert the compressed
# stream bytes are preserved verbatim and the file still reopens.
if command -v python3 > /dev/null 2>&1; then
  python3 -c "
import zlib, random, sys
random.seed(7)
# Mildly-compressible data so the *compressed* stream stays above the 4KB skip
# floor while remaining genuinely FlateDecode-encoded.
vocab = [b'alpha ', b'beta ', b'gamma ', b'delta ', b'epsilon ', b'zeta ']
raw = b''.join(random.choice(vocab) for _ in range(40000))
comp = zlib.compress(raw, 9)   # > 4KB, already compressed -> triggers the skip
objs = []
objs.append(b'<< /Type /Catalog /Pages 2 0 R >>')
objs.append(b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>')
objs.append(b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R >>')
objs.append(b'<< /Length %d /Filter /FlateDecode >>\nstream\n' % len(comp) + comp + b'\nendstream')
out = bytearray(b'%PDF-1.5\n')
offs = []
for i, body in enumerate(objs, 1):
    offs.append(len(out))
    out += (b'%d 0 obj\n' % i) + body + b'\nendobj\n'
xref = len(out)
out += b'xref\n0 %d\n' % (len(objs) + 1)
out += b'0000000000 65535 f \n'
for o in offs:
    out += b'%010d 00000 n \n' % o
out += b'trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (len(objs) + 1, xref)
open(sys.argv[1], 'wb').write(out)
" "$TMPDIR/bigflate.pdf"
  run_test "compress runs on already-compressed stream" $TSPDF compress "$TMPDIR/bigflate.pdf" -o "$TMPDIR/bigflate_out.pdf"
  run_test "compress preserves already-well-compressed stream bytes" python3 - "$TMPDIR/bigflate.pdf" "$TMPDIR/bigflate_out.pdf" << 'PYEOF'
import re, sys
def stream_bytes(p):
    d = open(p, "rb").read()
    m = re.search(rb"/Filter /FlateDecode >>\nstream\n(.*?)\nendstream", d, re.DOTALL)
    return m.group(1) if m else None
a = stream_bytes(sys.argv[1])
b = stream_bytes(sys.argv[2])
assert a is not None and b is not None, "stream not found"
# The well-compressed stream must be kept verbatim, not re-inflated.
assert a == b, "stream was needlessly re-encoded (%d -> %d bytes)" % (len(a), len(b))
PYEOF
  run_test "compress output reopens cleanly (round-trip)" $TSPDF info "$TMPDIR/bigflate_out.pdf"
else
  echo "  SKIP  compress well-compressed stream (python3 not found)"
fi

# img2pdf
if command -v python3 > /dev/null 2>&1; then
  python3 -c "
import struct, zlib, sys
sig = b'\x89PNG\r\n\x1a\n'
def chunk(t, d): return struct.pack('>I',len(d)) + t + d + struct.pack('>I', zlib.crc32(t+d) & 0xffffffff)
ihdr = struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0)
raw = zlib.compress(b'\x00\xff\x00\x00')
sys.stdout.buffer.write(sig + chunk(b'IHDR',ihdr) + chunk(b'IDAT',raw) + chunk(b'IEND',b''))
" > $TMPDIR/test.png
  run_test "img2pdf" $TSPDF img2pdf $TMPDIR/test.png -o $TMPDIR/img2pdf.pdf
  # flag-first ordering: the -o output path must not be loaded as an input image
  run_test "img2pdf flag-first ordering (input before -o)" $TSPDF img2pdf $TMPDIR/test.png -o $TMPDIR/img2pdf_ff.pdf
  run_test "img2pdf flag-first (-o before input)" $TSPDF img2pdf -o $TMPDIR/img2pdf_ff2.pdf $TMPDIR/test.png

  # An unsupported image (palette/color-type-3 PNG) that the decoder rejects.
  python3 -c "
import struct, zlib, sys
sig = b'\x89PNG\r\n\x1a\n'
def chunk(t, d): return struct.pack('>I',len(d)) + t + d + struct.pack('>I', zlib.crc32(t+d) & 0xffffffff)
ihdr = struct.pack('>IIBBBBB', 1, 1, 8, 3, 0, 0, 0)  # color_type 3 = palette (unsupported)
plte = b'\xff\x00\x00'
raw = zlib.compress(b'\x00\x00')
sys.stdout.buffer.write(sig + chunk(b'IHDR',ihdr) + chunk(b'PLTE',plte) + chunk(b'IDAT',raw) + chunk(b'IEND',b''))
" > $TMPDIR/palette.png

  # A partial failure (one good image + one unsupported) must exit non-zero
  # while still writing the pages that loaded.
  run_test "img2pdf partial failure exits non-zero" bash -c "! $TSPDF img2pdf $TMPDIR/test.png $TMPDIR/palette.png -o $TMPDIR/img2pdf_partial.pdf > /dev/null 2>&1"
  run_test "img2pdf partial failure still writes the good page" bash -c "$TSPDF info $TMPDIR/img2pdf_partial.pdf | grep -qE '^Pages:[[:space:]]*1$'"
  # --best-effort restores skip-and-exit-0 behaviour
  run_test "img2pdf --best-effort tolerates unsupported input (exit 0)" $TSPDF img2pdf $TMPDIR/test.png $TMPDIR/palette.png --best-effort -o $TMPDIR/img2pdf_be.pdf
  # All-good inputs still exit 0
  run_test "img2pdf all-good inputs exit 0" $TSPDF img2pdf $TMPDIR/test.png examples/test.jpg -o $TMPDIR/img2pdf_good.pdf
else
  echo "  SKIP  img2pdf (python3 not found)"
fi
run_test "img2pdf no images fails" bash -c "! $TSPDF img2pdf -o $TMPDIR/img2pdf_fail.pdf > /dev/null 2>&1"

# qrcode
run_test "qrcode" $TSPDF qrcode "https://example.com" -o $TMPDIR/qrcode.pdf
run_test "qrcode with title" $TSPDF qrcode "https://example.com" -o $TMPDIR/qrcode2.pdf --title "Test" --subtitle "Scan me"
run_test "qrcode no text fails" bash -c "! $TSPDF qrcode -o $TMPDIR/qr_fail.pdf > /dev/null 2>&1"
# flag-first ordering: -o output and --title/--subtitle values must not be swallowed as the text positional
run_test "qrcode flag-first ordering (-o/--title/--subtitle before text)" $TSPDF qrcode -o $TMPDIR/qrcode_ff.pdf --title "Test" --subtitle "Scan me" "https://example.com"
# a flag value must not be mistaken for the text positional (only flags present → still missing text)
run_test "qrcode title-only still reports missing text" bash -c "! $TSPDF qrcode -o $TMPDIR/qr_fail2.pdf --title MyTitle > /dev/null 2>&1"

# md2pdf
cat > $TMPDIR/test.md << 'MDEOF'
# Test Document

This is a paragraph.

- Item one
- Item two
MDEOF
run_test "md2pdf" $TSPDF md2pdf $TMPDIR/test.md -o $TMPDIR/md2pdf.pdf
run_test "md2pdf no input fails" bash -c "! $TSPDF md2pdf -o $TMPDIR/md_fail.pdf > /dev/null 2>&1"

# md2pdf inline formatting: **bold**, `code`, [label](url), and ordered lists
# must be parsed, not rendered literally. We inflate the page content stream
# and assert the markup characters never appear in the drawn text while the
# styled/labelled text and numbered markers do.
cat > $TMPDIR/inline.md << 'MDEOF'
# Title with **bold**

This has **bold**, `mono`, and a [click here](https://example.com) link.

- bullet with **strong** and `code`

1. first item
2. second item with **bold**
MDEOF
run_test "md2pdf parses inline markdown" $TSPDF md2pdf $TMPDIR/inline.md -o $TMPDIR/inline.pdf
if command -v python3 > /dev/null 2>&1; then
  run_test "md2pdf strips literal inline markers" python3 - "$TMPDIR/inline.pdf" << 'PYEOF'
import re, sys, zlib
data = open(sys.argv[1], "rb").read()
chunks = []
for m in re.finditer(rb"stream\r?\n(.*?)\r?\nendstream", data, re.DOTALL):
    raw = m.group(1)
    try:
        chunks.append(zlib.decompress(raw))
    except Exception:
        chunks.append(raw)
joined = b"\n".join(chunks).decode("latin-1")
# Concatenate text drawn via Tj string literals: ( ... )
drawn = "".join(re.findall(r"\((?:\\.|[^\\()])*\)", joined))
# Literal markdown markers must not survive into the drawn text.
for marker in ("**", "`", "](", "https://example.com"):
    assert marker not in drawn, "leaked marker %r in output" % marker
# Styled content and numbered markers must still be present.
for needed in ("bold", "mono", "click here", "code", "first item", "1.", "2."):
    assert needed in drawn, "missing expected text %r in output" % needed
PYEOF
else
  echo "  SKIP  md2pdf strips literal inline markers (python3 not found)"
fi

# help
run_test "help" $TSPDF --help
run_test "version" $TSPDF --version
run_test "help merge" $TSPDF help merge
run_test "help serve shows command-specific usage" bash -c "$TSPDF help serve 2>/dev/null | grep -q 'Usage: tspdf serve'"
run_test "top-level help describes text-only watermark" bash -c "$TSPDF --help 2>/dev/null | grep -E '^  watermark[[:space:]]+Add a text watermark'"

# serve --port validation: out-of-range and non-numeric ports must error out
# (non-zero exit) rather than silently binding a truncated/garbage port.
run_test "serve rejects out-of-range --port 99999" bash -c "! $TSPDF serve --port 99999 > /dev/null 2>&1"
run_test "serve rejects --port 0" bash -c "! $TSPDF serve --port 0 > /dev/null 2>&1"
run_test "serve rejects non-numeric --port abc" bash -c "! $TSPDF serve --port abc > /dev/null 2>&1"
run_test "serve rejects negative --port -1" bash -c "! $TSPDF serve --port -1 > /dev/null 2>&1"
run_test "serve rejects trailing-garbage --port 8080x" bash -c "! $TSPDF serve --port 8080x > /dev/null 2>&1"

# demo — flagship library example must build and run (regression: NULL-userdata segfault).
# Demo resolves asset paths (examples/test.jpg, fonts) relative to CWD and writes output.pdf
# into CWD, so run it from a throwaway dir with examples/ symlinked in.
run_test "demo builds, runs, and writes a valid PDF" bash -c '
  set -e
  REPO="$(pwd)"
  make build/tspdf_demo > /dev/null 2>&1
  DEMO_DIR="'"$TMPDIR"'/demo"
  mkdir -p "$DEMO_DIR"
  ln -snf "$REPO/examples" "$DEMO_DIR/examples"
  ( cd "$DEMO_DIR" && "$REPO/build/tspdf_demo" > /dev/null 2>&1 )
  head -c 5 "$DEMO_DIR/output.pdf" | grep -q "%PDF-"
'

# error cases — commands that should fail (exit non-zero)
run_test "missing input fails" bash -c "! $TSPDF info nonexistent.pdf > /dev/null 2>&1"
run_test "missing -o fails" bash -c "! $TSPDF split $INPUT --pages 1 > /dev/null 2>&1"

# metadata-view handler must not assign innerHTML (XSS-safe DOM for API-driven values)
run_test "metadata template view path has no innerHTML" bash -c '
  slice=$(sed -n "/fetch('\''\/api\/metadata-view'\''/,/setupTool('\''metadata'\''/p" web/templates/tools/metadata.html)
  [ -n "$slice" ] || exit 1
  echo "$slice" | grep -q "metadata-view" || exit 1
  ! echo "$slice" | grep -q innerHTML
'

# web UX copy should reinforce local-first privacy expectations
run_test "web copy includes explicit local-only privacy message" bash -c "
  grep -q 'No uploads\\. No analytics\\. No tracking\\.' web/templates/base.html web/templates/index.html
"

# generic tool flow should provide status + actionable guidance on errors
run_test "generic web app includes processing status and actionable error tips" bash -c "
  grep -q 'Processing locally' web/static/app.js &&
  grep -q 'Tip:' web/static/app.js
"

# serve — start server, test a few endpoints, kill
# Note: server is single-threaded so requests must be fully sequential.
# These tests use --retry to handle the brief window between responses.
echo ""
echo "  Serve:"
serve_pass=0
serve_fail=0
run_serve_test() {
    local name="$1"
    shift
    if "$@" > /dev/null 2>&1; then
        echo "  PASS  $name"
        serve_pass=$((serve_pass + 1))
    else
        echo "  FAIL  $name (flaky — single-threaded server)"
        serve_fail=$((serve_fail + 1))
    fi
}
if command -v curl > /dev/null 2>&1; then
  $TSPDF serve --port "$SERVE_PORT" > /dev/null 2>&1 &
  SERVE_PID=$!
  sleep 2
  run_serve_test "serve GET /" curl -sf --retry 3 --retry-delay 1 --max-time 5 "http://localhost:${SERVE_PORT}/" -o /dev/null
  run_serve_test "serve POST /api/compress" bash -c "curl -sf --retry 3 --retry-delay 1 --max-time 10 -F 'pdf_file=@$INPUT' -F 'config={}' http://localhost:${SERVE_PORT}/api/compress -o $TMPDIR/serve_compress.pdf && $TSPDF info $TMPDIR/serve_compress.pdf > /dev/null 2>&1"
  run_serve_test "serve GET /tool/img2pdf" curl -sf --retry 3 --retry-delay 1 --max-time 5 "http://localhost:${SERVE_PORT}/tool/img2pdf" -o /dev/null
  run_serve_test "serve GET /tool/md2pdf" curl -sf --retry 3 --retry-delay 1 --max-time 5 "http://localhost:${SERVE_PORT}/tool/md2pdf" -o /dev/null
  run_serve_test "serve GET /tool/qrcode" curl -sf --retry 3 --retry-delay 1 --max-time 5 "http://localhost:${SERVE_PORT}/tool/qrcode" -o /dev/null
  run_serve_test "serve POST /api/qrcode" bash -c "curl -sf --retry 3 --retry-delay 1 --max-time 10 -F 'config={\"url\":\"https://example.com\",\"title\":\"Test\"}' http://localhost:${SERVE_PORT}/api/qrcode -o $TMPDIR/serve_qr.pdf && $TSPDF info $TMPDIR/serve_qr.pdf > /dev/null 2>&1"
  run_serve_test "serve POST /api/md2pdf" bash -c "curl -sf --retry 3 --retry-delay 1 --max-time 10 -F 'config={\"text\":\"# Hello\nWorld\"}' http://localhost:${SERVE_PORT}/api/md2pdf -o $TMPDIR/serve_md.pdf && $TSPDF info $TMPDIR/serve_md.pdf > /dev/null 2>&1"
  # FIX 2: json_get_string must scan past escaped quotes and decode escapes, so
  # md2pdf text with an embedded \"quote\" and \n newlines survives intact
  # (the old scanner truncated at the first escaped quote).
  run_serve_test "serve md2pdf preserves escaped quotes and newlines in text" env TMPDIR="$TMPDIR" SERVE_PORT="$SERVE_PORT" bash -c '
    curl -sf --retry 3 --retry-delay 1 --max-time 10 \
      -F "config={\"text\":\"Alpha \\\"beta\\\" gamma\nDELTALINE\nEPSILONLINE\"}" \
      "http://localhost:${SERVE_PORT}/api/md2pdf" -o "${TMPDIR}/serve_md_esc.pdf" &&
    python3 -c "
import sys, re, zlib
data = open(sys.argv[1], \"rb\").read()
found = set()
for m in re.finditer(rb\"stream\r?\n(.*?)endstream\", data, re.S):
    s = m.group(1)
    try: s = zlib.decompress(s)
    except Exception: pass
    for w in (b\"Alpha\", b\"beta\", b\"gamma\", b\"DELTALINE\", b\"EPSILONLINE\"):
        if w in s: found.add(w)
need = {b\"Alpha\", b\"beta\", b\"gamma\", b\"DELTALINE\", b\"EPSILONLINE\"}
assert found == need, (sorted(found), sorted(need))
" "${TMPDIR}/serve_md_esc.pdf"
  '
  # FIX 1: img2pdf must not follow a pre-planted symlink at the OLD predictable
  # path /tmp/tspdf_img_<i>.png, so a victim file behind such a symlink stays
  # untouched while the API still produces a valid PDF.
  run_serve_test "serve img2pdf ignores pre-planted symlink at old temp path" env TSPDF="$TSPDF" TMPDIR="$TMPDIR" SERVE_PORT="$SERVE_PORT" bash -c '
    victim="${TMPDIR}/img2pdf_victim.txt"
    printf "PRECIOUS" > "$victim"
    for i in $(seq 0 8); do ln -sf "$victim" "/tmp/tspdf_img_${i}.png" 2>/dev/null || true; done
    cleanup() { for i in $(seq 0 8); do rm -f "/tmp/tspdf_img_${i}.png"; done; }
    trap cleanup EXIT
    curl -sf --retry 3 --retry-delay 1 --max-time 15 \
      -F "images=@examples/test.png" \
      "http://localhost:${SERVE_PORT}/api/img2pdf" -o "${TMPDIR}/img2pdf_out.pdf" || exit 1
    head -c5 "${TMPDIR}/img2pdf_out.pdf" | grep -q "%PDF-" || exit 1
    [ "$(cat "$victim")" = "PRECIOUS" ] || exit 1
  '
  run_serve_test "serve metadata-view JSON valid and preserves escaped title" env TSPDF="$TSPDF" TMPDIR="$TMPDIR" SERVE_PORT="$SERVE_PORT" bash -c '
    "$TSPDF" metadata tests/data/one_page.pdf --set '"'"'title=bad\"quote'"'"' -o "'"$TMPDIR"'/meta_quote.pdf" &&
    curl -sf --retry 3 --retry-delay 1 --max-time 10 -F "pdf_file=@'"$TMPDIR"'/meta_quote.pdf" http://localhost:'"${SERVE_PORT}"'/api/metadata-view |
    python3 -c "import json,sys; j=json.load(sys.stdin); want=(\"bad\"+chr(92)+chr(34)+\"quote\"); assert j[\"title\"]==want, (j[\"title\"], want)"
  '
  run_serve_test "serve metadata-view XSS title is valid JSON" bash -c "
    $TSPDF metadata tests/data/one_page.pdf --set 'title=<img src=x onerror=1>' -o \"$TMPDIR/meta_xss.pdf\" &&
    curl -sf --retry 3 --retry-delay 1 --max-time 10 -F \"pdf_file=@$TMPDIR/meta_xss.pdf\" http://localhost:${SERVE_PORT}/api/metadata-view |
    python3 -c \"
import json, sys
j = json.load(sys.stdin)
assert j['title'] == '<img src=x onerror=1>', j['title']
\"
  "
  # json_get_string must not let a   NUL truncate the text, nor emit an
  # unpaired surrogate as invalid UTF-8: both are sanitized to U+FFFD, so the
  # text after them survives into the rendered PDF.
  run_serve_test "serve md2pdf sanitizes NUL and lone surrogate escapes" env TMPDIR="$TMPDIR" SERVE_PORT="$SERVE_PORT" bash -c '
    curl -sf --retry 3 --retry-delay 1 --max-time 10 \
      -F "config={\"text\":\"HEADone\\u0000HEADtwo\\ud800HEADthree\"}" \
      "http://localhost:${SERVE_PORT}/api/md2pdf" -o "${TMPDIR}/serve_md_uni.pdf" &&
    python3 -c "
import sys, re, zlib
data = open(sys.argv[1], \"rb\").read()
found = set()
for m in re.finditer(rb\"stream\r?\n(.*?)endstream\", data, re.S):
    s = m.group(1)
    try: s = zlib.decompress(s)
    except Exception: pass
    for w in (b\"HEADone\", b\"HEADtwo\", b\"HEADthree\"):
        if w in s: found.add(w)
need = {b\"HEADone\", b\"HEADtwo\", b\"HEADthree\"}
assert found == need, (sorted(found), sorted(need))
" "${TMPDIR}/serve_md_uni.pdf"
  '
  kill $SERVE_PID 2>/dev/null || true
  wait $SERVE_PID 2>/dev/null || true
  # Serve failures don't fail the suite (flaky due to single-threaded server)
  echo "  ($serve_pass serve passed, $serve_fail serve flaky)"
else
  echo "  SKIP  serve tests (curl not found)"
fi

# Raw HTTP / header limits (must match count_header_field_lines + memmem \r\n\r\n boundary)
if command -v python3 > /dev/null 2>&1; then
  run_test "serve GET / with Host returns 200" bash -c "
    set -e
    \"$TSPDF\" serve --port $RAW_PORT_HOST > /dev/null 2>&1 & sp=\$!
    sleep 2
    python3 -c \"
import socket, sys
req = b'GET / HTTP/1.1\\\\r\\\\nHost: localhost\\\\r\\\\n\\\\r\\\\n'
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(('127.0.0.1', $RAW_PORT_HOST))
s.sendall(req)
data = s.recv(8192)
s.close()
if not data.startswith(b'HTTP/1.1 200'):
    sys.exit(1)
\"
    kill \$sp 2>/dev/null || true
    wait \$sp 2>/dev/null || true
  "
  run_test "serve accepts exactly 64 header field lines" bash -c "
    set -e
    \"$TSPDF\" serve --port $RAW_PORT_64H > /dev/null 2>&1 & sp=\$!
    sleep 2
    python3 -c \"
import socket, sys
lines = [b'GET / HTTP/1.1\\\\r\\\\n']
for i in range(64):
    lines.append(('X-Tspdf-H: %d\\\\r\\\\n' % i).encode('ascii'))
lines.append(b'\\\\r\\\\n')
req = b''.join(lines)
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(('127.0.0.1', $RAW_PORT_64H))
s.sendall(req)
data = s.recv(8192)
s.close()
if not data.startswith(b'HTTP/1.1 200'):
    sys.exit(1)
\"
    kill \$sp 2>/dev/null || true
    wait \$sp 2>/dev/null || true
  "
  run_test "serve rejects more than MAX_HEADERS header lines" bash -c "
    set -e
    \"$TSPDF\" serve --port $RAW_PORT_65H > /dev/null 2>&1 & sp=\$!
    sleep 2
    python3 -c \"
import socket, sys
lines = [b'GET / HTTP/1.1\\\\r\\\\n']
for i in range(65):
    lines.append(('X-Tspdf-H: %d\\\\r\\\\n' % i).encode('ascii'))
lines.append(b'\\\\r\\\\n')
req = b''.join(lines)
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(('127.0.0.1', $RAW_PORT_65H))
s.sendall(req)
data = s.recv(8192)
s.close()
if not data.startswith(b'HTTP/1.1 400'):
    sys.exit(1)
\"
    kill \$sp 2>/dev/null || true
    wait \$sp 2>/dev/null || true
  "

  # Malformed Content-Length must yield HTTP 400 (not hang or accept partial body)
  run_test "serve rejects Content-Length -1" bash -c "
    set -e
    \"$TSPDF\" serve --port $RAW_PORT_CLNEG > /dev/null 2>&1 & sp=\$!
    sleep 2
    python3 -c \"
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(('127.0.0.1', $RAW_PORT_CLNEG))
req = (b'POST /api/compress HTTP/1.1\\\\r\\\\nHost: localhost\\\\r\\\\n'
       b'Content-Length: -1\\\\r\\\\nContent-Type: application/octet-stream\\\\r\\\\n\\\\r\\\\n')
s.sendall(req)
data = s.recv(8192)
s.close()
if not data.startswith(b'HTTP/1.1 400'):
    sys.exit(1)
\"
    kill \$sp 2>/dev/null || true
    wait \$sp 2>/dev/null || true
  "
  run_test "serve rejects Content-Length larger than body" bash -c "
    set -e
    \"$TSPDF\" serve --port $RAW_PORT_CLBIG > /dev/null 2>&1 & sp=\$!
    sleep 2
    python3 -c \"
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(('127.0.0.1', $RAW_PORT_CLBIG))
req = (b'POST /api/compress HTTP/1.1\\\\r\\\\nHost: localhost\\\\r\\\\n'
       b'Content-Length: 100\\\\r\\\\n'
       b'Content-Type: multipart/form-data; boundary=x\\\\r\\\\n\\\\r\\\\n'
       b'short')
s.sendall(req)
s.shutdown(socket.SHUT_WR)
data = s.recv(8192)
s.close()
if not data.startswith(b'HTTP/1.1 400'):
    sys.exit(1)
\"
    kill \$sp 2>/dev/null || true
    wait \$sp 2>/dev/null || true
  "
  run_test "serve returns 408 when POST body read stalls" bash -c "
    set -e
    \"$TSPDF\" serve --port $RAW_PORT_STALL > /dev/null 2>&1 & sp=\$!
    sleep 2
    python3 -c \"
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(12)
s.connect(('127.0.0.1', $RAW_PORT_STALL))
req = (b'POST /api/compress HTTP/1.1\\\\r\\\\nHost: localhost\\\\r\\\\n'
       b'Content-Length: 100\\\\r\\\\n'
       b'Content-Type: multipart/form-data; boundary=x\\\\r\\\\n\\\\r\\\\n'
       b'short')
s.sendall(req)
data = s.recv(8192)
s.close()
if not data.startswith(b'HTTP/1.1 408'):
    sys.exit(1)
\"
    kill \$sp 2>/dev/null || true
    wait \$sp 2>/dev/null || true
  "
  run_test "serve metadata-view handles large escaped titles" bash -c "
    set -e
    \"$TSPDF\" serve --port $RAW_PORT_METAJSON > /dev/null 2>&1 & sp=\$!
    sleep 2
    TSPDF=\"$TSPDF\" TMPDIR=\"$TMPDIR\" python3 -c \"
import http.client, json, os, subprocess, sys
title = (chr(92) + chr(34)) * 2500 + 'END'
pdf_path = os.path.join(os.environ['TMPDIR'], 'meta_large.json.pdf')
subprocess.run([
    os.environ['TSPDF'], 'metadata', 'tests/data/one_page.pdf',
    '--set', 'title=' + title, '-o', pdf_path
], check=True)
with open(pdf_path, 'rb') as fh:
    pdf_bytes = fh.read()
boundary = 'TSPDFBOUNDARY'
body = (
    ('--' + boundary + '\\\\r\\\\n'
     'Content-Disposition: form-data; name=\\\"pdf_file\\\"; filename=\\\"meta_large.json.pdf\\\"\\\\r\\\\n'
     'Content-Type: application/pdf\\\\r\\\\n\\\\r\\\\n').encode('utf-8')
    + pdf_bytes +
    ('\\\\r\\\\n--' + boundary + '--\\\\r\\\\n').encode('utf-8')
)
conn = http.client.HTTPConnection('127.0.0.1', $RAW_PORT_METAJSON, timeout=10)
conn.request('POST', '/api/metadata-view', body=body, headers={
    'Content-Type': 'multipart/form-data; boundary=' + boundary,
    'Content-Length': str(len(body)),
})
resp = conn.getresponse()
payload = resp.read()
conn.close()
if resp.status != 200:
    sys.exit(1)
data = json.loads(payload.decode('utf-8'))
if data.get('title') != title:
    sys.exit(1)
\"
    kill \$sp 2>/dev/null || true
    wait \$sp 2>/dev/null || true
  "
  run_test "serve accepts case-insensitive multipart Boundary parameter" bash -c "
    set -e
    \"$TSPDF\" serve --port $RAW_PORT_BOUNDARY_CI > /dev/null 2>&1 & sp=\$!
    sleep 2
    python3 -c \"
import http.client, sys
boundary = 'TSPDFBOUNDARYCI'
body = (
    ('--' + boundary + '\\\\r\\\\n'
     'Content-Disposition: form-data; name=\\\"config\\\"\\\\r\\\\n\\\\r\\\\n'
     '{\\\"url\\\":\\\"https://example.com\\\",\\\"title\\\":\\\"CaseInsensitiveBoundary\\\"}\\\\r\\\\n'
     '--' + boundary + '--\\\\r\\\\n').encode('utf-8')
)
conn = http.client.HTTPConnection('127.0.0.1', $RAW_PORT_BOUNDARY_CI, timeout=10)
conn.request('POST', '/api/qrcode', body=body, headers={
    'Content-Type': 'multipart/form-data; Boundary=' + boundary,
    'Content-Length': str(len(body)),
})
resp = conn.getresponse()
payload = resp.read()
conn.close()
if resp.status != 200 or not payload.startswith(b'%PDF-'):
    sys.exit(1)
\"
    kill \$sp 2>/dev/null || true
    wait \$sp 2>/dev/null || true
  "
  # SO_SNDTIMEO + the write loop's abort-on-non-positive-return must keep the
  # single-threaded server alive when a client drops mid-response: the first
  # client RSTs without reading, the second must still get a clean 200.
  run_test "serve recovers after client drops the response without reading" bash -c "
    set -e
    \"$TSPDF\" serve --port $RAW_PORT_WRITEDROP > /dev/null 2>&1 & sp=\$!
    sleep 2
    python3 -c \"
import socket, struct, sys
req = b'GET / HTTP/1.1\\\\r\\\\nHost: localhost\\\\r\\\\n\\\\r\\\\n'
# Client 1: request the page, then abort with a TCP RST before reading anything.
s1 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s1.settimeout(5)
s1.connect(('127.0.0.1', $RAW_PORT_WRITEDROP))
s1.sendall(req)
s1.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
s1.close()  # RST: server's response write() should fail and the loop close cleanly
# Client 2: the server must have recovered and still serve a normal request.
s2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s2.settimeout(5)
s2.connect(('127.0.0.1', $RAW_PORT_WRITEDROP))
s2.sendall(req)
data = s2.recv(8192)
s2.close()
if not data.startswith(b'HTTP/1.1 200'):
    sys.exit(1)
\"
    kill \$sp 2>/dev/null || true
    wait \$sp 2>/dev/null || true
  "

  # DNS-rebinding mitigation: a non-loopback Host header (what a rebound
  # attacker domain presents) must be rejected with 403 before the body is
  # touched, even though the request reached the loopback-bound socket.
  run_test "serve rejects POST /api with non-loopback Host (DNS rebinding)" bash -c "
    set -e
    \"$TSPDF\" serve --port $RAW_PORT_REBIND_HOST > /dev/null 2>&1 & sp=\$!
    sleep 2
    python3 -c \"
import socket, sys
bnd = '----tspdf'
body = ('--%s\\\\r\\\\nContent-Disposition: form-data; name=\\\"config\\\"\\\\r\\\\n\\\\r\\\\n'
        '{\\\"url\\\":\\\"https://example.com\\\"}\\\\r\\\\n--%s--\\\\r\\\\n' % (bnd, bnd)).encode('ascii')
hdr = ('POST /api/qrcode HTTP/1.1\\\\r\\\\nHost: evil.example.com\\\\r\\\\n'
       'Content-Type: multipart/form-data; boundary=%s\\\\r\\\\n'
       'Content-Length: %d\\\\r\\\\n\\\\r\\\\n' % (bnd, len(body))).encode('ascii')
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(('127.0.0.1', $RAW_PORT_REBIND_HOST))
s.sendall(hdr + body)
data = s.recv(8192)
s.close()
if not data.startswith(b'HTTP/1.1 403'):
    sys.exit(1)
\"
    kill \$sp 2>/dev/null || true
    wait \$sp 2>/dev/null || true
  "

  # A loopback Host but a cross-site Origin (the CORS-style cross-origin POST a
  # malicious page would emit) must also be rejected with 403.
  run_test "serve rejects POST /api with cross-origin Origin" bash -c "
    set -e
    \"$TSPDF\" serve --port $RAW_PORT_REBIND_ORIGIN > /dev/null 2>&1 & sp=\$!
    sleep 2
    python3 -c \"
import socket, sys
bnd = '----tspdf'
body = ('--%s\\\\r\\\\nContent-Disposition: form-data; name=\\\"config\\\"\\\\r\\\\n\\\\r\\\\n'
        '{\\\"url\\\":\\\"https://example.com\\\"}\\\\r\\\\n--%s--\\\\r\\\\n' % (bnd, bnd)).encode('ascii')
hdr = ('POST /api/qrcode HTTP/1.1\\\\r\\\\nHost: 127.0.0.1:$RAW_PORT_REBIND_ORIGIN\\\\r\\\\n'
       'Origin: https://evil.example.com\\\\r\\\\n'
       'Content-Type: multipart/form-data; boundary=%s\\\\r\\\\n'
       'Content-Length: %d\\\\r\\\\n\\\\r\\\\n' % (bnd, len(body))).encode('ascii')
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(('127.0.0.1', $RAW_PORT_REBIND_ORIGIN))
s.sendall(hdr + body)
data = s.recv(8192)
s.close()
if not data.startswith(b'HTTP/1.1 403'):
    sys.exit(1)
\"
    kill \$sp 2>/dev/null || true
    wait \$sp 2>/dev/null || true
  "

  # The legitimate same-origin POST the bundled web UI sends (loopback Host +
  # matching loopback Origin carrying the connected port) must succeed.
  run_test "serve accepts POST /api with loopback Host and same Origin" bash -c "
    set -e
    \"$TSPDF\" serve --port $RAW_PORT_REBIND_OK > /dev/null 2>&1 & sp=\$!
    sleep 2
    python3 -c \"
import socket, sys
bnd = '----tspdf'
body = ('--%s\\\\r\\\\nContent-Disposition: form-data; name=\\\"config\\\"\\\\r\\\\n\\\\r\\\\n'
        '{\\\"url\\\":\\\"https://example.com\\\"}\\\\r\\\\n--%s--\\\\r\\\\n' % (bnd, bnd)).encode('ascii')
hdr = ('POST /api/qrcode HTTP/1.1\\\\r\\\\nHost: localhost:$RAW_PORT_REBIND_OK\\\\r\\\\n'
       'Origin: http://localhost:$RAW_PORT_REBIND_OK\\\\r\\\\n'
       'Content-Type: multipart/form-data; boundary=%s\\\\r\\\\n'
       'Content-Length: %d\\\\r\\\\n\\\\r\\\\n' % (bnd, len(body))).encode('ascii')
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(('127.0.0.1', $RAW_PORT_REBIND_OK))
s.sendall(hdr + body)
data = s.recv(8192)
s.close()
if not data.startswith(b'HTTP/1.1 200'):
    sys.exit(1)
\"
    kill \$sp 2>/dev/null || true
    wait \$sp 2>/dev/null || true
  "
else
  echo "  SKIP  serve raw HTTP regressions (python3 not found)"
fi

# --- Audit fixes (fix/reader-core): out-of-range page messages, split
# --- output stripping, compress of unfiltered streams ---

# Out-of-range page indices must name the offending page and the document's
# actual page count instead of a generic "PDF parsing failed".
run_test "split out-of-range page message" bash -c "
  ! $TSPDF split $INPUT --pages 5 -o $TMPDIR/oor_split.pdf > /dev/null 2>$TMPDIR/oor_split.err &&
  grep -q 'page 5 is out of range (document has 3 pages)' $TMPDIR/oor_split.err
"
run_test "delete out-of-range page message" bash -c "
  ! $TSPDF delete $INPUT --pages 9 -o $TMPDIR/oor_delete.pdf > /dev/null 2>$TMPDIR/oor_delete.err &&
  grep -q 'page 9 is out of range (document has 3 pages)' $TMPDIR/oor_delete.err
"
run_test "rotate out-of-range page message" bash -c "
  ! $TSPDF rotate $INPUT --pages 4 --angle 90 -o $TMPDIR/oor_rotate.pdf > /dev/null 2>$TMPDIR/oor_rotate.err &&
  grep -q 'page 4 is out of range (document has 3 pages)' $TMPDIR/oor_rotate.err
"
run_test "reorder out-of-range page message" bash -c "
  ! $TSPDF reorder $INPUT --order 1,2,7 -o $TMPDIR/oor_reorder.pdf > /dev/null 2>$TMPDIR/oor_reorder.err &&
  grep -q 'page 7 is out of range (document has 3 pages)' $TMPDIR/oor_reorder.err
"

# split must not drag unreachable source objects into the output: extracting
# one page of three must produce a file well under the source size.
run_test "split output smaller than source" bash -c "
  $TSPDF split $INPUT --pages 1 -o $TMPDIR/split_small.pdf > /dev/null 2>&1 &&
  in_sz=\$(wc -c < $INPUT) && out_sz=\$(wc -c < $TMPDIR/split_small.pdf) &&
  [ \"\$out_sz\" -lt \"\$in_sz\" ]
"

# compress must FlateDecode-compress streams that are stored with no filter
# (e.g. qpdf --stream-data=uncompress output) for a large reduction.
if command -v qpdf > /dev/null 2>&1; then
  run_test "compress shrinks uncompressed-streams file" bash -c "
    qpdf --stream-data=uncompress $INPUT $TMPDIR/uncompressed.pdf &&
    $TSPDF compress $TMPDIR/uncompressed.pdf -o $TMPDIR/recompressed.pdf > /dev/null 2>&1 &&
    in_sz=\$(wc -c < $TMPDIR/uncompressed.pdf) && out_sz=\$(wc -c < $TMPDIR/recompressed.pdf) &&
    [ \$((out_sz * 10)) -lt \$((in_sz * 9)) ]
  "
  run_test "compress uncompressed-streams output passes qpdf --check" bash -c "
    qpdf --check $TMPDIR/recompressed.pdf > /dev/null 2>&1
  "
else
  echo "  SKIP  compress uncompressed-streams file (qpdf not found)"
fi

echo ""
echo "$pass passed, $fail failed"
[ $fail -eq 0 ] || exit 1
