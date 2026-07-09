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

  # An unsupported image (interlaced/Adam7 PNG) that the decoder rejects.
  python3 -c "
import struct, zlib, sys
sig = b'\x89PNG\r\n\x1a\n'
def chunk(t, d): return struct.pack('>I',len(d)) + t + d + struct.pack('>I', zlib.crc32(t+d) & 0xffffffff)
ihdr = struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 1)  # interlace 1 = Adam7 (unsupported)
raw = zlib.compress(b'\x00\xff\x00\x00')
sys.stdout.buffer.write(sig + chunk(b'IHDR',ihdr) + chunk(b'IDAT',raw) + chunk(b'IEND',b''))
" > $TMPDIR/interlaced.png

  # A partial failure (one good image + one unsupported) must exit non-zero
  # while still writing the pages that loaded.
  run_test "img2pdf partial failure exits non-zero" bash -c "! $TSPDF img2pdf $TMPDIR/test.png $TMPDIR/interlaced.png -o $TMPDIR/img2pdf_partial.pdf > /dev/null 2>&1"
  run_test "img2pdf partial failure still writes the good page" bash -c "$TSPDF info $TMPDIR/img2pdf_partial.pdf | grep -qE '^Pages:[[:space:]]*1$'"
  # --best-effort restores skip-and-exit-0 behaviour
  run_test "img2pdf --best-effort tolerates unsupported input (exit 0)" $TSPDF img2pdf $TMPDIR/test.png $TMPDIR/interlaced.png --best-effort -o $TMPDIR/img2pdf_be.pdf
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
# ---------------------------------------------------------------
# encoding/i18n regressions (fix/encoding)
# ---------------------------------------------------------------

# metadata: non-ASCII values must round-trip through the CLI display path and
# be stored as a BOM-prefixed UTF-16BE text string (not raw UTF-8 bytes).
run_test "metadata non-ASCII round-trip (UTF-16BE with BOM)" bash -c "
  set -e
  \"$TSPDF\" metadata \"$INPUT\" --set title='Prüfbericht – Änderungen' -o \"$TMPDIR/enc_meta.pdf\"
  \"$TSPDF\" metadata \"$TMPDIR/enc_meta.pdf\" | grep -qF 'Prüfbericht – Änderungen'
  LC_ALL=C grep -aq '<FEFF' \"$TMPDIR/enc_meta.pdf\"
"

# Output PDFs must be stamped with the real project name + version.
run_test "producer stamp is tspdf + version" bash -c "
  set -e
  \"$TSPDF\" rotate \"$INPUT\" -o \"$TMPDIR/enc_prod.pdf\" --angle 90
  if LC_ALL=C grep -aqF '(tspr)' \"$TMPDIR/enc_prod.pdf\"; then exit 1; fi
  LC_ALL=C grep -aqE '/Producer \(tspdf [0-9]+\.[0-9]+\.[0-9]+\)' \"$TMPDIR/enc_prod.pdf\"
"

# watermark: cp1252-representable text must succeed and the content stream
# must carry WinAnsi bytes (0x96 en-dash, 0xFC u-umlaut), not UTF-8 sequences.
run_test "watermark emits cp1252 bytes in content stream" bash -c "
  set -e
  \"$TSPDF\" watermark \"$INPUT\" -o \"$TMPDIR/enc_wm.pdf\" --text 'Vertraulich – geprüft'
  LC_ALL=C grep -aqF \"\$(printf 'Vertraulich \\226 gepr\\374ft')\" \"$TMPDIR/enc_wm.pdf\"
  if LC_ALL=C grep -aqF \"\$(printf 'gepr\\303\\274ft')\" \"$TMPDIR/enc_wm.pdf\"; then exit 1; fi
  if command -v qpdf > /dev/null 2>&1; then
    qpdf --check \"$TMPDIR/enc_wm.pdf\"
  fi
"

# watermark: text outside WinAnsi must fail with an error naming the character.
run_test "watermark rejects non-WinAnsi text with clear error" bash -c "
  set -e
  if \"$TSPDF\" watermark \"$INPUT\" -o \"$TMPDIR/enc_wm_cjk.pdf\" --text '機密' 2> \"$TMPDIR/enc_wm_err.txt\"; then
    exit 1
  fi
  grep -q 'U+6A5F' \"$TMPDIR/enc_wm_err.txt\"
"

# md2pdf: Latin-script UTF-8 must be drawn as cp1252 bytes (small content
# streams are stored uncompressed, so the raw output is grep-able), with no
# warnings for cp1252-clean input.
run_test "md2pdf renders Latin-1 punctuation via WinAnsi" bash -c "
  set -e
  printf '# Prüfbericht\n\nnaïve café — dash\n' > \"$TMPDIR/enc_doc.md\"
  \"$TSPDF\" md2pdf \"$TMPDIR/enc_doc.md\" -o \"$TMPDIR/enc_doc.pdf\" 2> \"$TMPDIR/enc_doc_err.txt\"
  test ! -s \"$TMPDIR/enc_doc_err.txt\"
  LC_ALL=C grep -aqF \"\$(printf 'na\\357ve caf\\351 \\227 dash')\" \"$TMPDIR/enc_doc.pdf\"
  if LC_ALL=C grep -aqF \"\$(printf 'caf\\303\\251')\" \"$TMPDIR/enc_doc.pdf\"; then exit 1; fi
  if command -v qpdf > /dev/null 2>&1; then
    qpdf --check \"$TMPDIR/enc_doc.pdf\"
  fi
"

# md2pdf: characters outside WinAnsi become '?' with a per-line warning, and
# the document still converts (one emoji must not fail a whole README).
run_test "md2pdf substitutes non-WinAnsi chars with warning" bash -c "
  set -e
  printf 'ok line\nemoji \\360\\237\\230\\200 here\n' > \"$TMPDIR/enc_emoji.md\"
  \"$TSPDF\" md2pdf \"$TMPDIR/enc_emoji.md\" -o \"$TMPDIR/enc_emoji.pdf\" 2> \"$TMPDIR/enc_emoji_err.txt\"
  grep -q 'line 2' \"$TMPDIR/enc_emoji_err.txt\"
  grep -q 'U+1F600' \"$TMPDIR/enc_emoji_err.txt\"
  LC_ALL=C grep -aqF 'emoji ? here' \"$TMPDIR/enc_emoji.pdf\"
"

# md2pdf: the help text mentions the WinAnsi limitation.
run_test "md2pdf help mentions WinAnsi limitation" bash -c "
  \"$TSPDF\" md2pdf --help | grep -qi 'WinAnsi'
"
# ============================================================
# cli-media track: img2pdf aspect ratio, palette PNGs, passwords
# ============================================================

# img2pdf must scale images to fit the A4 content box preserving aspect ratio,
# centered. Verified by parsing /MediaBox and the image XObject cm matrix out
# of the output ("w 0 0 h x y cm").
if command -v python3 > /dev/null 2>&1; then
  gen_rgb_png() {  # gen_rgb_png <width> <height> <path>
    python3 -c "
import struct, zlib, sys
w, h = int(sys.argv[1]), int(sys.argv[2])
sig = b'\x89PNG\r\n\x1a\n'
def chunk(t, d): return struct.pack('>I',len(d)) + t + d + struct.pack('>I', zlib.crc32(t+d) & 0xffffffff)
ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
raw = zlib.compress(b''.join(b'\x00' + b'\x40\x80\xc0' * w for _ in range(h)))
open(sys.argv[3], 'wb').write(sig + chunk(b'IHDR',ihdr) + chunk(b'IDAT',raw) + chunk(b'IEND',b''))
" "$1" "$2" "$3"
  }
  check_img2pdf_aspect() {  # check_img2pdf_aspect <pdf> <img_width> <img_height>
    python3 - "$1" "$2" "$3" << 'PYEOF'
import re, sys, zlib
pdf = open(sys.argv[1], 'rb').read()
iw, ih = float(sys.argv[2]), float(sys.argv[3])
m = re.search(rb'/MediaBox \[([\d. ]+)\]', pdf)
assert m, 'no /MediaBox found'
mb = [float(v) for v in m.group(1).split()]
pw, ph = mb[2] - mb[0], mb[3] - mb[1]
streams = []
for s in re.finditer(rb'stream\r?\n(.*?)\r?\nendstream', pdf, re.DOTALL):
    raw = s.group(1)
    try: streams.append(zlib.decompress(raw))
    except Exception: streams.append(raw)
text = b'\n'.join(streams).decode('latin-1')
cm = re.search(r'([\d.]+) 0 0 ([\d.]+) ([\d.]+) ([\d.]+) cm', text)
assert cm, 'no image cm matrix found'
w, h, x, y = (float(v) for v in cm.groups())
margin = 36.0
aw, ah = pw - 2 * margin, ph - 2 * margin
# aspect ratio preserved (within the 4-decimal print rounding)
assert abs(w / h - iw / ih) < 0.01 * (iw / ih), (w, h, iw, ih)
# fits inside the content box and fills the constraining axis
assert w <= aw + 0.01 and h <= ah + 0.01, (w, h, aw, ah)
assert abs(w - aw) < 0.01 or abs(h - ah) < 0.01, (w, h, aw, ah)
# centered in the content box
assert abs((x - margin) - (aw - w) / 2) < 0.01, (x, w, aw)
assert abs((y - margin) - (ah - h) / 2) < 0.01, (y, h, ah)
PYEOF
  }
  gen_rgb_png 320 200 "$TMPDIR/wide.png"
  gen_rgb_png 100 400 "$TMPDIR/tall.png"
  run_test "img2pdf converts wide image" $TSPDF img2pdf $TMPDIR/wide.png -o $TMPDIR/wide.pdf
  run_test "img2pdf preserves wide aspect ratio, centered" check_img2pdf_aspect $TMPDIR/wide.pdf 320 200
  run_test "img2pdf converts tall image" $TSPDF img2pdf $TMPDIR/tall.png -o $TMPDIR/tall.pdf
  run_test "img2pdf preserves tall aspect ratio, centered" check_img2pdf_aspect $TMPDIR/tall.pdf 100 400

  # Palette (color type 3) PNGs are now decoded, including sub-byte depths.
  python3 -c "
import struct, zlib, sys
sig = b'\x89PNG\r\n\x1a\n'
def chunk(t, d): return struct.pack('>I',len(d)) + t + d + struct.pack('>I', zlib.crc32(t+d) & 0xffffffff)
ihdr = struct.pack('>IIBBBBB', 2, 2, 8, 3, 0, 0, 0)  # 2x2, 8-bit palette
plte = b'\xff\x00\x00\x00\xff\x00\x00\x00\xff'
raw = zlib.compress(b'\x00\x00\x01\x00\x02\x01')  # rows: [0,1] [2,1]
open(sys.argv[1], 'wb').write(sig + chunk(b'IHDR',ihdr) + chunk(b'PLTE',plte) + chunk(b'IDAT',raw) + chunk(b'IEND',b''))
" "$TMPDIR/pal8.png"
  python3 -c "
import struct, zlib, sys
sig = b'\x89PNG\r\n\x1a\n'
def chunk(t, d): return struct.pack('>I',len(d)) + t + d + struct.pack('>I', zlib.crc32(t+d) & 0xffffffff)
ihdr = struct.pack('>IIBBBBB', 9, 1, 1, 3, 0, 0, 0)  # 9x1, 1-bit palette
plte = b'\x00\x00\x00\xff\xff\xff'
raw = zlib.compress(b'\x00\xaa\x80')  # 101010101 packed MSB-first
open(sys.argv[1], 'wb').write(sig + chunk(b'IHDR',ihdr) + chunk(b'PLTE',plte) + chunk(b'IDAT',raw) + chunk(b'IEND',b''))
" "$TMPDIR/pal1.png"
  run_test "img2pdf accepts 8-bit palette PNG" $TSPDF img2pdf $TMPDIR/pal8.png -o $TMPDIR/pal8.pdf
  run_test "img2pdf accepts 1-bit palette PNG" $TSPDF img2pdf $TMPDIR/pal1.png -o $TMPDIR/pal1.pdf
  if command -v qpdf > /dev/null 2>&1; then
    run_test "img2pdf palette PNG output passes qpdf --check" qpdf --check $TMPDIR/pal8.pdf
    run_test "img2pdf aspect-fit output passes qpdf --check" qpdf --check $TMPDIR/wide.pdf
  else
    echo "  SKIP  img2pdf qpdf --check (qpdf not found)"
  fi

  # Interlaced PNGs stay rejected, with an error message that says so.
  python3 -c "
import struct, zlib, sys
sig = b'\x89PNG\r\n\x1a\n'
def chunk(t, d): return struct.pack('>I',len(d)) + t + d + struct.pack('>I', zlib.crc32(t+d) & 0xffffffff)
ihdr = struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 1)  # interlace 1 = Adam7
raw = zlib.compress(b'\x00\xff\x00\x00')
open(sys.argv[1], 'wb').write(sig + chunk(b'IHDR',ihdr) + chunk(b'IDAT',raw) + chunk(b'IEND',b''))
" "$TMPDIR/adam7.png"
  run_test "img2pdf reports interlaced PNG clearly" bash -c "$TSPDF img2pdf $TMPDIR/adam7.png -o $TMPDIR/adam7.pdf 2>&1 | grep -q 'interlaced PNG not supported'"
else
  echo "  SKIP  img2pdf aspect/palette tests (python3 not found)"
fi

# encrypt/decrypt --password-file: first line of the file (trailing newline
# stripped), or stdin when the path is '-'. Passwords must never be required
# on argv.
printf 'filesecret\n' > $TMPDIR/pw.txt
run_test "encrypt --password-file" $TSPDF encrypt $INPUT -o $TMPDIR/encpf.pdf --password-file $TMPDIR/pw.txt
run_test "decrypt --password-file (newline stripped)" $TSPDF decrypt $TMPDIR/encpf.pdf -o $TMPDIR/decpf.pdf --password-file $TMPDIR/pw.txt
run_test "encrypt --password-file matches plain --password" $TSPDF decrypt $TMPDIR/encpf.pdf -o $TMPDIR/decpf2.pdf --password filesecret
run_test "encrypt --password-file - (stdin)" bash -c "echo stdinsecret | $TSPDF encrypt $INPUT -o $TMPDIR/encstdin.pdf --password-file -"
run_test "decrypt --password-file - (stdin)" bash -c "echo stdinsecret | $TSPDF decrypt $TMPDIR/encstdin.pdf -o $TMPDIR/decstdin.pdf --password-file -"
run_test "encrypt missing --password-file path fails" bash -c "! $TSPDF encrypt $INPUT -o $TMPDIR/encnopf.pdf --password-file $TMPDIR/no_such_pw.txt > /dev/null 2>&1"

# Omitted password with non-TTY stdin: no prompt possible, so the commands
# must fail with an error pointing at --password / --password-file.
run_test "encrypt no password non-TTY fails" bash -c "! $TSPDF encrypt $INPUT -o $TMPDIR/encnopw.pdf < /dev/null > /dev/null 2>&1"
run_test "encrypt no password non-TTY mentions --password-file" bash -c "$TSPDF encrypt $INPUT -o $TMPDIR/encnopw.pdf < /dev/null 2>&1 | grep -q -- '--password-file'"
run_test "decrypt no password non-TTY fails" bash -c "! $TSPDF decrypt $TMPDIR/encpf.pdf -o $TMPDIR/decnopw.pdf < /dev/null > /dev/null 2>&1"
run_test "decrypt no password non-TTY mentions --password-file" bash -c "$TSPDF decrypt $TMPDIR/encpf.pdf -o $TMPDIR/decnopw.pdf < /dev/null 2>&1 | grep -q -- '--password-file'"

# --owner-password-file: owner password read from a file. Validate with qpdf,
# which can open the file with the owner password.
if command -v qpdf > /dev/null 2>&1; then
  printf 'ownersecret\n' > $TMPDIR/opw.txt
  run_test "encrypt --owner-password-file" $TSPDF encrypt $INPUT -o $TMPDIR/encopf.pdf --password usersecret --owner-password-file $TMPDIR/opw.txt
  run_test "owner password from file opens the PDF (qpdf)" qpdf --password=ownersecret --check $TMPDIR/encopf.pdf
else
  echo "  SKIP  encrypt --owner-password-file (qpdf not found)"
fi

# Help text documents the new password options.
run_test "encrypt help mentions --password-file" bash -c "$TSPDF encrypt --help | grep -q -- '--password-file'"
run_test "decrypt help mentions --password-file" bash -c "$TSPDF decrypt --help | grep -q -- '--password-file'"

# --- Packaging/distribution tests (build flags, install layout, headers) ---

# The Makefile must honor packager-injected CPPFLAGS/LDFLAGS and keep the
# required flags when CFLAGS is overridden (dpkg-buildflags, makepkg, abuild
# and nix all inject these and lint when they are dropped). `make -n` keeps
# the checks cheap: no rebuild, just the commands make would run.
run_test "make honors CPPFLAGS on compile lines" bash -c '
  make -n BUILDDIR=/tmp/tspdf-nbuild CPPFLAGS=-DTSPDF_CPPFLAGS_PROBE | grep -q -- -DTSPDF_CPPFLAGS_PROBE'

run_test "make honors LDFLAGS on link lines" bash -c '
  make -n BUILDDIR=/tmp/tspdf-nbuild LDFLAGS=-Wl,--tspdf-ldflags-probe | grep -q -- --tspdf-ldflags-probe'

run_test "required flags survive a CFLAGS override" bash -c '
  make -n BUILDDIR=/tmp/tspdf-nbuild CFLAGS=-O3 | grep -q -- -std=c11'

# A fully static Linux build is what the release workflow ships; glibc-only
# feature, so skip elsewhere (macOS has no static libSystem).
if [ "$(uname -s)" = "Linux" ]; then
  run_test "make LDFLAGS=-static links a static binary" bash -c '
    set -e
    make BUILDDIR="'"$TMPDIR"'/staticbuild" LDFLAGS=-static > /dev/null 2>&1
    file "'"$TMPDIR"'/staticbuild/tspdf" | grep -q "statically linked"'
else
  echo "  SKIP  make LDFLAGS=-static links a static binary (Linux only)"
fi

# `make install` must print a PATH hint when $(PREFIX)/bin is not on PATH
# (README recommends PREFIX=~/.local, which is not on PATH by default on macOS).
run_test "make install hints when PREFIX/bin not on PATH" bash -c '
  make install DESTDIR="'"$TMPDIR"'/hintstage" PREFIX=/opt/tspdf-hint-probe 2>/dev/null \
    | grep -q "/opt/tspdf-hint-probe/bin is not on your PATH"'

run_test "make install stays quiet when PREFIX/bin is on PATH" bash -c '
  set -e
  PATH="/opt/tspdf-hint-probe/bin:$PATH" make install DESTDIR="'"$TMPDIR"'/hintstage2" PREFIX=/opt/tspdf-hint-probe \
    > "'"$TMPDIR"'/hint-quiet-out.txt" 2>/dev/null
  ! grep -q "not on your PATH" "'"$TMPDIR"'/hint-quiet-out.txt"'

# The man page ships with `make install`.
run_test "make install places tspdf.1 in man1" bash -c '
  test -f "'"$TMPDIR"'/hintstage/opt/tspdf-hint-probe/share/man/man1/tspdf.1"'

# Installed-layout acceptance: stage `make install-lib` and compile a C and a
# C++ translation unit that include BOTH umbrella headers against the staged
# tree. This proves the overlay header works installed and that every header
# it pulls in is extern "C" guarded.
INSTALL_STAGE="$TMPDIR/libstage"
run_test "install-lib stages overlay header with the reader headers" bash -c '
  set -e
  make install-lib DESTDIR="'"$INSTALL_STAGE"'" PREFIX=/usr > /dev/null
  test -f "'"$INSTALL_STAGE"'/usr/include/tspdf/tspdf_overlay.h"
  test -f "'"$INSTALL_STAGE"'/usr/include/tspdf/reader/tspr_overlay.h"'

run_test "staged headers compile as C" bash -c '
  printf "#include <tspdf/tspdf.h>\n#include <tspdf/tspdf_overlay.h>\nint main(void){return 0;}\n" > "'"$TMPDIR"'/hdr_c.c"
  cc -std=c11 -Wall -Wextra -Werror -fsyntax-only -I "'"$INSTALL_STAGE"'/usr/include" "'"$TMPDIR"'/hdr_c.c"'

if command -v c++ > /dev/null 2>&1; then
  run_test "staged headers compile as C++" bash -c '
    printf "#include <tspdf/tspdf.h>\n#include <tspdf/tspdf_overlay.h>\nint main(){return 0;}\n" > "'"$TMPDIR"'/hdr_cpp.cc"
    c++ -x c++ -Wall -Wextra -Werror -fsyntax-only -I "'"$INSTALL_STAGE"'/usr/include" "'"$TMPDIR"'/hdr_cpp.cc"'
else
  echo "  SKIP  staged headers compile as C++ (c++ not found)"
fi

# --- Text extraction (tspdf text) ---

run_test "text extracts page text" bash -c "$TSPDF text $INPUT | grep -q 'Page 1'"
run_test "text outputs all pages by default" bash -c "[ \$($TSPDF text $INPUT | grep -c 'Page') -eq 3 ]"
run_test "text separates pages with form feed" bash -c "[ \$($TSPDF text $INPUT | tr -cd '\f' | wc -c) -eq 2 ]"
run_test "text --pages selects pages" bash -c "$TSPDF text $INPUT --pages 2 | grep -q 'Page 2'"
run_test "text --pages excludes others" bash -c "! $TSPDF text $INPUT --pages 2 | grep -q 'Page 1'"
run_test "text -o writes file" bash -c "$TSPDF text $INPUT --pages 1 -o $TMPDIR/text_out.txt && grep -q 'Page 1' $TMPDIR/text_out.txt"
run_test "text out-of-range page message" bash -c "$TSPDF text $INPUT --pages 9 2>&1 | grep -q 'page 9 is out of range (document has 3 pages)'"
run_test "text invalid page range fails" bash -c "! $TSPDF text $INPUT --pages bogus > /dev/null 2>&1"
run_test "text help mentions --pages" bash -c "$TSPDF text --help | grep -q -- '--pages'"
run_test "text help mentions --password" bash -c "$TSPDF text --help | grep -q -- '--password'"
run_test "text missing input fails" bash -c "! $TSPDF text $TMPDIR/no_such_input.pdf > /dev/null 2>&1"
run_test "text listed in main help" bash -c "$TSPDF --help | grep -q '^  text'"

# --- Doc trees: bookmarks and form fields across merge/split ---
# Verified against qpdf's JSON view of the outputs; skipped when qpdf is not
# installed. tests/data/outline_form.pdf: OF-CH1 -> p1 (child OF-CH1-SUB ->
# p2), OF-CH2 -> p3; text field of_text on p1, checkbox of_check on p3.
OUTLINE_INPUT=tests/data/outline_form.pdf
if command -v qpdf > /dev/null 2>&1 && [ -f "$OUTLINE_INPUT" ]; then
  run_test "merge preserves bookmarks and form fields" bash -c '
    set -e
    "'"$TSPDF"'" merge tests/data/three_pages.pdf "'"$OUTLINE_INPUT"'" -o "'"$TMPDIR"'/dt_merged.pdf"
    qpdf --check "'"$TMPDIR"'/dt_merged.pdf"
    qpdf --json --json-key=outlines "'"$TMPDIR"'/dt_merged.pdf" | grep -q "OF-CH1"
    qpdf --json --json-key=outlines "'"$TMPDIR"'/dt_merged.pdf" | grep -q "OF-CH2"
    qpdf --json --json-key=acroform "'"$TMPDIR"'/dt_merged.pdf" | grep -q "of_text"
    qpdf --json --json-key=acroform "'"$TMPDIR"'/dt_merged.pdf" | grep -q "of_check"'

  run_test "split keeps only the kept pages' bookmarks and fields" bash -c '
    set -e
    "'"$TSPDF"'" split "'"$OUTLINE_INPUT"'" --pages 1-2 -o "'"$TMPDIR"'/dt_split.pdf"
    qpdf --check "'"$TMPDIR"'/dt_split.pdf"
    qpdf --json --json-key=outlines "'"$TMPDIR"'/dt_split.pdf" | grep -q "OF-CH1-SUB"
    ! qpdf --json --json-key=outlines "'"$TMPDIR"'/dt_split.pdf" | grep -q "OF-CH2"
    qpdf --json --json-key=acroform "'"$TMPDIR"'/dt_split.pdf" | grep -q "of_text"
    ! qpdf --json --json-key=acroform "'"$TMPDIR"'/dt_split.pdf" | grep -q "of_check"'
else
  echo "  SKIP  merge preserves bookmarks and form fields (qpdf or fixture missing)"
  echo "  SKIP  split keeps only the kept pages' bookmarks and fields (qpdf or fixture missing)"
fi

echo ""
echo "$pass passed, $fail failed"
[ $fail -eq 0 ] || exit 1
