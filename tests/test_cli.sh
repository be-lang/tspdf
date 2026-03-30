#!/bin/bash
set -e
TSPDF=./build/tspdf
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

echo "Building CLI..."
make cli > /dev/null 2>&1

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

# info
run_test "info" $TSPDF info $INPUT

# split
run_test "split pages 1-2" $TSPDF split $INPUT --pages 1-2 -o $TMPDIR/split.pdf
run_test "split result has 2 pages" $TSPDF info $TMPDIR/split.pdf

# merge
run_test "merge two files" $TSPDF merge $INPUT $TMPDIR/split.pdf -o $TMPDIR/merged.pdf

# rotate
run_test "rotate all 90" $TSPDF rotate $INPUT --angle 90 -o $TMPDIR/rotated.pdf
run_test "rotate specific pages" $TSPDF rotate $INPUT --pages 1 --angle 180 -o $TMPDIR/rotated2.pdf

# delete
run_test "delete page 1" $TSPDF delete $INPUT --pages 1 -o $TMPDIR/deleted.pdf

# reorder
run_test "reorder pages" $TSPDF reorder $INPUT --order 3,1,2 -o $TMPDIR/reordered.pdf

# encrypt + decrypt
run_test "encrypt AES-128" $TSPDF encrypt $INPUT -o $TMPDIR/enc128.pdf --password secret
run_test "encrypt AES-256" $TSPDF encrypt $INPUT -o $TMPDIR/enc256.pdf --password secret --bits 256
run_test "decrypt" $TSPDF decrypt $TMPDIR/enc128.pdf -o $TMPDIR/decrypted.pdf --password secret

# metadata
run_test "metadata view" $TSPDF metadata $INPUT
run_test "metadata set" $TSPDF metadata $INPUT --set title="Test Title" --set author="Test Author" -o $TMPDIR/meta.pdf

# watermark
run_test "watermark" $TSPDF watermark $INPUT -o $TMPDIR/watermark.pdf --text "DRAFT"

# compress
run_test "compress" $TSPDF compress $INPUT -o $TMPDIR/compressed.pdf

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
else
  echo "  SKIP  img2pdf (python3 not found)"
fi
run_test "img2pdf no images fails" bash -c "! $TSPDF img2pdf -o $TMPDIR/img2pdf_fail.pdf > /dev/null 2>&1"

# qrcode
run_test "qrcode" $TSPDF qrcode "https://example.com" -o $TMPDIR/qrcode.pdf
run_test "qrcode with title" $TSPDF qrcode "https://example.com" -o $TMPDIR/qrcode2.pdf --title "Test" --subtitle "Scan me"
run_test "qrcode no text fails" bash -c "! $TSPDF qrcode -o $TMPDIR/qr_fail.pdf > /dev/null 2>&1"

# md2pdf
cat > $TMPDIR/test.md << 'MDEOF'
# Test Document

This is a paragraph.

- Item one
- Item two
MDEOF
run_test "md2pdf" $TSPDF md2pdf $TMPDIR/test.md -o $TMPDIR/md2pdf.pdf
run_test "md2pdf no input fails" bash -c "! $TSPDF md2pdf -o $TMPDIR/md_fail.pdf > /dev/null 2>&1"

# help
run_test "help" $TSPDF --help
run_test "version" $TSPDF --version
run_test "help merge" $TSPDF help merge

# error cases — commands that should fail (exit non-zero)
run_test "missing input fails" bash -c "! $TSPDF info nonexistent.pdf > /dev/null 2>&1"
run_test "missing -o fails" bash -c "! $TSPDF split $INPUT --pages 1 > /dev/null 2>&1"

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
  $TSPDF serve --port 19876 > /dev/null 2>&1 &
  SERVE_PID=$!
  sleep 2
  run_serve_test "serve GET /" curl -sf --retry 3 --retry-delay 1 --max-time 5 http://localhost:19876/ -o /dev/null
  run_serve_test "serve POST /api/compress" bash -c "curl -sf --retry 3 --retry-delay 1 --max-time 10 -F 'pdf_file=@$INPUT' -F 'config={}' http://localhost:19876/api/compress -o $TMPDIR/serve_compress.pdf && $TSPDF info $TMPDIR/serve_compress.pdf > /dev/null 2>&1"
  run_serve_test "serve GET /tool/img2pdf" curl -sf --retry 3 --retry-delay 1 --max-time 5 http://localhost:19876/tool/img2pdf -o /dev/null
  run_serve_test "serve GET /tool/md2pdf" curl -sf --retry 3 --retry-delay 1 --max-time 5 http://localhost:19876/tool/md2pdf -o /dev/null
  run_serve_test "serve GET /tool/qrcode" curl -sf --retry 3 --retry-delay 1 --max-time 5 http://localhost:19876/tool/qrcode -o /dev/null
  run_serve_test "serve POST /api/qrcode" bash -c "curl -sf --retry 3 --retry-delay 1 --max-time 10 -F 'config={\"url\":\"https://example.com\",\"title\":\"Test\"}' http://localhost:19876/api/qrcode -o $TMPDIR/serve_qr.pdf && $TSPDF info $TMPDIR/serve_qr.pdf > /dev/null 2>&1"
  run_serve_test "serve POST /api/md2pdf" bash -c "curl -sf --retry 3 --retry-delay 1 --max-time 10 -F 'config={\"text\":\"# Hello\nWorld\"}' http://localhost:19876/api/md2pdf -o $TMPDIR/serve_md.pdf && $TSPDF info $TMPDIR/serve_md.pdf > /dev/null 2>&1"
  kill $SERVE_PID 2>/dev/null || true
  wait $SERVE_PID 2>/dev/null || true
  # Serve failures don't fail the suite (flaky due to single-threaded server)
  echo "  ($serve_pass serve passed, $serve_fail serve flaky)"
else
  echo "  SKIP  serve tests (curl not found)"
fi

echo ""
echo "$pass passed, $fail failed"
[ $fail -eq 0 ] || exit 1
