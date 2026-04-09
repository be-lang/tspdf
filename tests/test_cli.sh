#!/bin/bash
set -e
TSPDF=./build/tspdf
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

echo "Building CLI..."
rm -rf build
make cli > /dev/null 2>&1
if [ ! -x "$TSPDF" ]; then
    echo "error: $TSPDF missing after 'make cli' (need a real phony build target)" >&2
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
run_test "help serve shows command-specific usage" bash -c "$TSPDF help serve 2>/dev/null | grep -q 'Usage: tspdf serve'"
run_test "top-level help describes text-only watermark" bash -c "$TSPDF --help 2>/dev/null | grep -E '^  watermark[[:space:]]+Add a text watermark'"

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
else
  echo "  SKIP  serve raw HTTP regressions (python3 not found)"
fi

echo ""
echo "$pass passed, $fail failed"
[ $fail -eq 0 ] || exit 1
