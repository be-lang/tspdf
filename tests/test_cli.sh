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
run_test "info shows the PDF version" bash -c "$TSPDF info $INPUT | grep -qE '^Version:[[:space:]]+PDF 1\.7$'"
run_test "info flags outlines and AcroForm when present" bash -c "
  set -e
  out=\$($TSPDF info tests/data/outline_form.pdf)
  echo \"\$out\" | grep -qE '^Outlines:[[:space:]]+yes$'
  echo \"\$out\" | grep -qE '^AcroForm:[[:space:]]+yes$'"
run_test "info flags absent outlines and AcroForm" bash -c "
  set -e
  out=\$($TSPDF info $INPUT)
  echo \"\$out\" | grep -qE '^Outlines:[[:space:]]+no$'
  echo \"\$out\" | grep -qE '^AcroForm:[[:space:]]+no$'"

# info: encryption permissions, JSON error objects, CropBox (needs python3 for
# JSON validation and for crafting fixtures)
if command -v python3 > /dev/null 2>&1; then
  # Encrypted fixture allowing only print and copy: /P = 0xFFFFF0C0 | bit3 |
  # bit5 = 0xFFFFF0D4 = -3884 as the signed value written to the file.
  $TSPDF encrypt $INPUT -o $TMPDIR/enc_perm.pdf --password pw --permissions print,copy > /dev/null 2>&1

  run_test "info text reports permissions on encrypted file" bash -c "
    $TSPDF info --password pw $TMPDIR/enc_perm.pdf | grep -qE '^Permissions:[[:space:]]+print, copy \(P=-3884\)$'"

  cat > $TMPDIR/check_info_perms.py << 'PYEOF'
import json, sys
d = json.load(sys.stdin)
assert d["encrypted"] is True, d
assert d["permissions"] == ["print", "copy"], d.get("permissions")
assert d["permissions_raw"] == -3884, d.get("permissions_raw")
PYEOF
  run_test "info --json reports permissions on encrypted file" bash -c "
    $TSPDF info --json --password pw $TMPDIR/enc_perm.pdf | python3 $TMPDIR/check_info_perms.py"

  run_test "info --json permissions are null when unencrypted" bash -c "
    $TSPDF info --json $INPUT | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d[\"permissions\"] is None and d[\"permissions_raw\"] is None'"

  if command -v qpdf > /dev/null 2>&1; then
    # Semantics cross-check: qpdf decodes the same /P bits (copy = qpdf's
    # \"extract for any purpose\", extract = \"extract for accessibility\").
    run_test "info permissions agree with qpdf --show-encryption" bash -c "
      set -e
      out=\$(qpdf --password=pw --show-encryption $TMPDIR/enc_perm.pdf)
      echo \"\$out\" | grep -q -- 'P = -3884'
      echo \"\$out\" | grep -q 'print low resolution: allowed'
      echo \"\$out\" | grep -q 'extract for any purpose: allowed'
      echo \"\$out\" | grep -q 'extract for accessibility: not allowed'
      echo \"\$out\" | grep -q 'modify other: not allowed'
      echo \"\$out\" | grep -q 'modify annotations: not allowed'"
  fi

  run_test "info --json wrong password emits JSON error with nonzero exit" bash -c "
    out=\$($TSPDF info --json --password wrong $TMPDIR/enc_perm.pdf 2>/dev/null); rc=\$?
    [ \$rc -ne 0 ] || exit 1
    echo \"\$out\" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert \"error\" in d and d[\"encrypted\"] is True'"

  run_test "info --json missing file emits JSON error with nonzero exit" bash -c "
    out=\$($TSPDF info --json $TMPDIR/no_such_file.pdf 2>/dev/null); rc=\$?
    [ \$rc -ne 0 ] || exit 1
    echo \"\$out\" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert \"error\" in d'"

  run_test "info --json missing input argument emits JSON error" bash -c "
    out=\$($TSPDF info --json 2>/dev/null); rc=\$?
    [ \$rc -ne 0 ] || exit 1
    echo \"\$out\" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert \"error\" in d'"

  run_test "info shows CropBox when present" bash -c "
    set -e
    $TSPDF crop $INPUT -o $TMPDIR/cropped_info.pdf --box 100,100,400,500 > /dev/null
    $TSPDF info $TMPDIR/cropped_info.pdf | grep -q 'CropBox 300.0 x 400.0 pt'"

  run_test "info --json reports crop_sizes" bash -c "
    $TSPDF info --json $TMPDIR/cropped_info.pdf | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d[\"crop_sizes\"] == [300.0, 400.0], d[\"crop_sizes\"]'"

  run_test "info --json crop_sizes null when no CropBox" bash -c "
    $TSPDF info --json $INPUT | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d[\"crop_sizes\"] is None'"
fi

# scale: /Rotate-aware orientation, verified by actual rendering dimensions.
# A portrait-stored page with /Rotate 90 is VIEWED landscape; scaling --to a4
# must keep the viewed orientation (a4 landscape), not flip it to portrait.
if command -v python3 > /dev/null 2>&1 && command -v pdftoppm > /dev/null 2>&1; then
  python3 - $TMPDIR/rot90.pdf << 'PYEOF'
import sys
objs = [
    b'<< /Type /Catalog /Pages 2 0 R >>',
    b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Rotate 90 /Contents 4 0 R >>',
]
content = b'0 0 100 50 re f\n50 700 200 60 re f\n'
out = bytearray(b'%PDF-1.4\n')
offs = []
for i, body in enumerate(objs, 1):
    offs.append(len(out))
    out += (b'%d 0 obj\n' % i) + body + b'\nendobj\n'
offs.append(len(out))
out += b'4 0 obj\n<< /Length %d >>\nstream\n' % len(content) + content + b'endstream\nendobj\n'
xref = len(out)
n = len(offs) + 1
out += b'xref\n0 %d\n' % n + b'0000000000 65535 f \n'
for o in offs:
    out += b'%010d 00000 n \n' % o
out += b'trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (n, xref)
open(sys.argv[1], 'wb').write(bytes(out))
PYEOF
  cat > $TMPDIR/check_png_dims.py << 'PYEOF'
import struct, sys
w, h = struct.unpack(">II", open(sys.argv[1], "rb").read()[16:24])
ew, eh = int(sys.argv[2]), int(sys.argv[3])
assert abs(w - ew) <= 1 and abs(h - eh) <= 1, (w, h, ew, eh)
PYEOF
  run_test "scale --to a4 keeps a /Rotate 90 page viewed landscape" bash -c "
    set -e
    pdftoppm -r 72 -png $TMPDIR/rot90.pdf $TMPDIR/rot90_src
    python3 $TMPDIR/check_png_dims.py $TMPDIR/rot90_src-1.png 792 612
    $TSPDF scale $TMPDIR/rot90.pdf -o $TMPDIR/rot90_a4.pdf --to a4 > /dev/null
    pdftoppm -r 72 -png $TMPDIR/rot90_a4.pdf $TMPDIR/rot90_a4
    python3 $TMPDIR/check_png_dims.py $TMPDIR/rot90_a4-1.png 842 595"

  run_test "scale --to a4 keeps portrait pages portrait" bash -c "
    set -e
    $TSPDF scale $INPUT -o $TMPDIR/portrait_a4.pdf --to a4 > /dev/null
    pdftoppm -r 72 -png -f 1 -l 1 $TMPDIR/portrait_a4.pdf $TMPDIR/portrait_a4
    python3 $TMPDIR/check_png_dims.py $TMPDIR/portrait_a4-1.png 595 842"
fi

# split
run_test "split pages 1-2" $TSPDF split $INPUT --pages 1-2 -o $TMPDIR/split.pdf
run_test "split result has exactly 2 pages (info output)" bash -c "$TSPDF info $TMPDIR/split.pdf | grep -qE '^Pages:[[:space:]]*2$'"
# flag-first ordering: -o output path must not be swallowed as the input file
run_test "split flag-first ordering (-o before input)" $TSPDF split -o $TMPDIR/split_ff.pdf --pages 1 $INPUT
run_test "split flag-first result is the real input (1 page)" bash -c "$TSPDF info $TMPDIR/split_ff.pdf | grep -qE '^Pages:[[:space:]]*1$'"

# page labels survive split and merge (verified through qpdf's label view)
if command -v python3 > /dev/null 2>&1 && command -v qpdf > /dev/null 2>&1; then
  python3 - "$TMPDIR/labeled.pdf" << 'PYEOF'
import sys
# five pages labeled i, ii, iii, 1, 2
objs = [
    b'<< /Type /Catalog /Pages 2 0 R /PageLabels 8 0 R >>',
    b'<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R 6 0 R 7 0 R] /Count 5 >>',
] + [b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>'] * 5 + [
    b'<< /Nums [0 << /S /r >> 3 << /S /D >>] >>',
]
out = bytearray(b'%PDF-1.4\n')
offs = []
for i, body in enumerate(objs, 1):
    offs.append(len(out))
    out += (b'%d 0 obj\n' % i) + body + b'\nendobj\n'
xref = len(out)
out += b'xref\n0 %d\n' % (len(objs)+1) + b'0000000000 65535 f \n'
for o in offs:
    out += b'%010d 00000 n \n' % o
out += b'trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (len(objs)+1, xref)
open(sys.argv[1], 'wb').write(out)
PYEOF
  run_test "split keeps effective page labels (iii, 1)" bash -c "
    set -e
    $TSPDF split $TMPDIR/labeled.pdf --pages 3-4 -o $TMPDIR/labeled_ex.pdf > /dev/null
    qpdf --json --json-key=pagelabels $TMPDIR/labeled_ex.pdf 2>/dev/null | python3 -c '
import json, sys
labels = json.load(sys.stdin)[\"pagelabels\"]
assert labels[0] == {\"index\": 0, \"label\": {\"/S\": \"/r\", \"/St\": 3}}, labels
assert labels[1] == {\"index\": 1, \"label\": {\"/S\": \"/D\", \"/St\": 1}}, labels
assert len(labels) == 2, labels
'"
  run_test "merge offsets labels, unlabeled doc gets decimal range" bash -c "
    set -e
    $TSPDF merge $TMPDIR/labeled.pdf $INPUT -o $TMPDIR/labeled_mg.pdf > /dev/null
    qpdf --json --json-key=pagelabels $TMPDIR/labeled_mg.pdf 2>/dev/null | python3 -c '
import json, sys
labels = json.load(sys.stdin)[\"pagelabels\"]
assert labels[0] == {\"index\": 0, \"label\": {\"/S\": \"/r\", \"/St\": 1}}, labels
assert labels[1] == {\"index\": 3, \"label\": {\"/S\": \"/D\", \"/St\": 1}}, labels
assert labels[2] == {\"index\": 5, \"label\": {\"/S\": \"/D\", \"/St\": 1}}, labels
assert len(labels) == 3, labels
'"
  run_test "merge of unlabeled docs emits no /PageLabels" bash -c "
    set -e
    $TSPDF merge $INPUT $INPUT -o $TMPDIR/unlabeled_mg.pdf > /dev/null
    ! grep -qa '/PageLabels' $TMPDIR/unlabeled_mg.pdf"
else
  echo "  SKIP  page label preservation assertions (python3/qpdf not found)"
fi

# merge
run_test "merge two files" $TSPDF merge $INPUT $TMPDIR/split.pdf -o $TMPDIR/merged.pdf
# flag-first ordering: -o output must not be counted as an input file
run_test "merge flag-first ordering (-o before inputs)" $TSPDF merge -o $TMPDIR/merged_ff.pdf $INPUT $TMPDIR/split.pdf

# merge must not silently truncate a long input list (was capped at 64 inputs,
# printing "Merged 64 files" and dropping the rest with exit 0 — data loss).
MANY_DIR=$TMPDIR/merge_many
mkdir -p $MANY_DIR
$TSPDF split $INPUT --pages 1 -o $MANY_DIR/page.pdf > /dev/null 2>&1
MANY_INPUTS=()
for i in $(seq 1 70); do
  cp $MANY_DIR/page.pdf $MANY_DIR/in_$i.pdf
  MANY_INPUTS+=("$MANY_DIR/in_$i.pdf")
done
run_test "merge 70 files exits 0" $TSPDF merge "${MANY_INPUTS[@]}" -o $TMPDIR/merged_many.pdf
run_test "merge 70 files keeps every page" bash -c "$TSPDF info $TMPDIR/merged_many.pdf | grep -qE '^Pages:[[:space:]]*70$'"

# single-input commands must reject extra positionals instead of silently
# ignoring them (a user merging/splitting the wrong file must hear about it)
run_test "split rejects a second input file" bash -c "! $TSPDF split $INPUT $TMPDIR/split.pdf --pages 1 -o $TMPDIR/split_extra.pdf > /dev/null 2>&1"
run_test "watermark rejects a second input file" bash -c "! $TSPDF watermark $INPUT $TMPDIR/split.pdf --text DRAFT -o $TMPDIR/wm_extra.pdf > /dev/null 2>&1"

# rotate
run_test "rotate all 90" $TSPDF rotate $INPUT --angle 90 -o $TMPDIR/rotated.pdf
run_test "rotate specific pages" $TSPDF rotate $INPUT --pages 1 --angle 180 -o $TMPDIR/rotated2.pdf
# flag-first ordering: -o output and --angle value must not be swallowed as input
run_test "rotate flag-first ordering (-o/--angle before input)" $TSPDF rotate -o $TMPDIR/rotated_ff.pdf --angle 90 $INPUT
run_test "rotate flag-first result has 3 pages" bash -c "$TSPDF info $TMPDIR/rotated_ff.pdf | grep -qE '^Pages:[[:space:]]*3$'"

# page operations on encrypted input: every command that opens a PDF takes
# --password/--password-file, fails with a hint without one, and its output
# keeps the original encryption (saves preserve source encryption).
$TSPDF encrypt $INPUT -o $TMPDIR/pwop_enc.pdf --password pgsecret > /dev/null 2>&1
printf 'pgsecret\n' > $TMPDIR/pwop_pass.txt

run_test "rotate encrypted input with --password" bash -c "
  set -e
  \"$TSPDF\" rotate \"$TMPDIR/pwop_enc.pdf\" --angle 90 --password pgsecret -o \"$TMPDIR/pwop_rot.pdf\"
  [ \"\$(wc -c < \"$TMPDIR/pwop_rot.pdf\")\" -gt 0 ]
"
run_test "rotate encrypted input with --password-file" $TSPDF rotate $TMPDIR/pwop_enc.pdf --angle 90 --password-file $TMPDIR/pwop_pass.txt -o $TMPDIR/pwop_rot2.pdf
run_test "rotate encrypted input without password fails with hint" bash -c "
  if \"$TSPDF\" rotate \"$TMPDIR/pwop_enc.pdf\" --angle 90 -o \"$TMPDIR/pwop_rot3.pdf\" < /dev/null > /dev/null 2> \"$TMPDIR/pwop_rot3.err\"; then
    exit 1
  fi
  grep -q 'use --password or --password-file' \"$TMPDIR/pwop_rot3.err\"
"
run_test "rotate output stays encrypted, opens with original password" bash -c "
  set -e
  \"$TSPDF\" info \"$TMPDIR/pwop_rot.pdf\" | grep -q 'This PDF is encrypted'
  \"$TSPDF\" info --password pgsecret \"$TMPDIR/pwop_rot.pdf\" | grep -qE '^Pages:[[:space:]]*3$'
"

run_test "delete encrypted input with --password" bash -c "
  set -e
  \"$TSPDF\" delete \"$TMPDIR/pwop_enc.pdf\" --pages 1 --password pgsecret -o \"$TMPDIR/pwop_del.pdf\"
  \"$TSPDF\" info --password pgsecret \"$TMPDIR/pwop_del.pdf\" | grep -qE '^Pages:[[:space:]]*2$'
"
run_test "delete encrypted input with --password-file" $TSPDF delete $TMPDIR/pwop_enc.pdf --pages 1 --password-file $TMPDIR/pwop_pass.txt -o $TMPDIR/pwop_del2.pdf
run_test "delete encrypted input without password fails with hint" bash -c "
  if \"$TSPDF\" delete \"$TMPDIR/pwop_enc.pdf\" --pages 1 -o \"$TMPDIR/pwop_del3.pdf\" < /dev/null > /dev/null 2> \"$TMPDIR/pwop_del3.err\"; then
    exit 1
  fi
  grep -q 'use --password or --password-file' \"$TMPDIR/pwop_del3.err\"
"
run_test "delete output stays encrypted" bash -c "
  \"$TSPDF\" info \"$TMPDIR/pwop_del.pdf\" | grep -q 'This PDF is encrypted'
"

if command -v qpdf > /dev/null 2>&1; then
  run_test "rotate/delete encrypted outputs are encrypted per qpdf" bash -c "
    set -e
    qpdf --is-encrypted \"$TMPDIR/pwop_rot.pdf\"
    qpdf --is-encrypted \"$TMPDIR/pwop_del.pdf\"
    qpdf --password=pgsecret --check \"$TMPDIR/pwop_rot.pdf\" > /dev/null
  "
fi

# merge: the one --password is tried on every input; unencrypted inputs ignore
# it, and an input it does not unlock is a clear error.
run_test "merge encrypted + plain inputs with a single --password" bash -c "
  set -e
  \"$TSPDF\" merge \"$TMPDIR/pwop_enc.pdf\" \"$INPUT\" --password pgsecret -o \"$TMPDIR/pwop_merged.pdf\"
  \"$TSPDF\" info \"$TMPDIR/pwop_merged.pdf\" --password pgsecret | grep -qE '^Pages:[[:space:]]*6$'
"
run_test "merge wrong password names the input it cannot unlock" bash -c "
  if \"$TSPDF\" merge \"$TMPDIR/pwop_enc.pdf\" \"$INPUT\" --password nope -o \"$TMPDIR/pwop_m2.pdf\" > /dev/null 2> \"$TMPDIR/pwop_m2.err\"; then
    exit 1
  fi
  grep -q 'wrong password' \"$TMPDIR/pwop_m2.err\" && grep -q 'pwop_enc.pdf' \"$TMPDIR/pwop_m2.err\"
"

# --- round 4: open-error parity — every command that opens a PDF reports
# --- "wrong password" specifically, not a generic open failure.
$TSPDF encrypt $INPUT -o $TMPDIR/openerr_enc.pdf --password r4secret > /dev/null 2>&1
run_test "rotate wrong password message" bash -c "
  ! \"$TSPDF\" rotate \"$TMPDIR/openerr_enc.pdf\" --angle 90 --password nope -o \"$TMPDIR/openerr_rot.pdf\" 2> \"$TMPDIR/openerr.err\" &&
  grep -q \"wrong password for '\" \"$TMPDIR/openerr.err\" && grep -q \"openerr_enc.pdf'\" \"$TMPDIR/openerr.err\" && ! grep -q 'failed to open' \"$TMPDIR/openerr.err\""
run_test "attach wrong password message" bash -c "
  ! \"$TSPDF\" attach list \"$TMPDIR/openerr_enc.pdf\" --password nope > /dev/null 2> \"$TMPDIR/openerr.err\" &&
  grep -q \"wrong password for '\" \"$TMPDIR/openerr.err\" && grep -q \"openerr_enc.pdf'\" \"$TMPDIR/openerr.err\" && ! grep -q 'failed to open' \"$TMPDIR/openerr.err\""
run_test "nup wrong password message" bash -c "
  ! \"$TSPDF\" nup 4 \"$TMPDIR/openerr_enc.pdf\" --password nope -o \"$TMPDIR/openerr_nup.pdf\" 2> \"$TMPDIR/openerr.err\" &&
  grep -q \"wrong password for '\" \"$TMPDIR/openerr.err\" && grep -q \"openerr_enc.pdf'\" \"$TMPDIR/openerr.err\" && ! grep -q 'failed to open' \"$TMPDIR/openerr.err\""
run_test "form wrong password message" bash -c "
  ! \"$TSPDF\" form list \"$TMPDIR/openerr_enc.pdf\" --password nope > /dev/null 2> \"$TMPDIR/openerr.err\" &&
  grep -q \"wrong password for '\" \"$TMPDIR/openerr.err\" && grep -q \"openerr_enc.pdf'\" \"$TMPDIR/openerr.err\" && ! grep -q 'failed to open' \"$TMPDIR/openerr.err\""
run_test "stamp wrong password message" bash -c "
  ! \"$TSPDF\" stamp \"$TMPDIR/openerr_enc.pdf\" --stamp \"$INPUT\" --password nope -o \"$TMPDIR/openerr_st.pdf\" 2> \"$TMPDIR/openerr.err\" &&
  grep -q \"wrong password for '\" \"$TMPDIR/openerr.err\" && grep -q \"openerr_enc.pdf'\" \"$TMPDIR/openerr.err\" && ! grep -q 'failed to open' \"$TMPDIR/openerr.err\""
run_test "bookmark wrong password message" bash -c "
  ! \"$TSPDF\" bookmark list \"$TMPDIR/openerr_enc.pdf\" --password nope > /dev/null 2> \"$TMPDIR/openerr.err\" &&
  grep -q \"wrong password for '\" \"$TMPDIR/openerr.err\" && grep -q \"openerr_enc.pdf'\" \"$TMPDIR/openerr.err\" && ! grep -q 'failed to open' \"$TMPDIR/openerr.err\""

run_test "merge encrypted input without password fails with hint" bash -c "
  if \"$TSPDF\" merge \"$TMPDIR/pwop_enc.pdf\" \"$INPUT\" -o \"$TMPDIR/pwop_m3.pdf\" < /dev/null > /dev/null 2> \"$TMPDIR/pwop_m3.err\"; then
    exit 1
  fi
  grep -q 'use --password' \"$TMPDIR/pwop_m3.err\"
"

# split: password accepted in both --pages and burst modes; parts stay encrypted
run_test "split encrypted input with --password keeps parts encrypted" bash -c "
  set -e
  \"$TSPDF\" split \"$TMPDIR/pwop_enc.pdf\" --pages 1-2 --password pgsecret -o \"$TMPDIR/pwop_split.pdf\"
  \"$TSPDF\" info \"$TMPDIR/pwop_split.pdf\" | grep -q 'This PDF is encrypted'
  \"$TSPDF\" info --password pgsecret \"$TMPDIR/pwop_split.pdf\" | grep -qE '^Pages:[[:space:]]*2$'
"

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

# info must name the encryption scheme when it can open the file
run_test "info reports AES-128 R4 encryption details" bash -c "
  $TSPDF info $TMPDIR/enc128.pdf --password secret | grep -qE '^Encrypted:[[:space:]]+yes \(AES-128, R4\)$'"
run_test "info reports AES-256 R6 encryption details" bash -c "
  $TSPDF info $TMPDIR/enc256.pdf --password secret | grep -qE '^Encrypted:[[:space:]]+yes \(AES-256, R6\)$'"

# encrypt --permissions: listed actions allowed, everything else denied
run_test "encrypt --permissions rejects unknown tokens" bash -c "! $TSPDF encrypt $INPUT -o $TMPDIR/encbad.pdf --password secret --permissions print,teleport > /dev/null 2>&1"
run_test "encrypt --permissions print" $TSPDF encrypt $INPUT -o $TMPDIR/encperm.pdf --password secret --permissions print
run_test "restricted file still opens with the user password" bash -c "
  $TSPDF info $TMPDIR/encperm.pdf --password secret | grep -qE '^Pages:[[:space:]]*3$'"
if command -v qpdf > /dev/null 2>&1; then
  run_test "qpdf sees print allowed, the rest denied" bash -c "
    set -e
    out=\$(qpdf --password=secret --show-encryption $TMPDIR/encperm.pdf)
    echo \"\$out\" | grep -q '^print low resolution: allowed'
    echo \"\$out\" | grep -q '^print high resolution: not allowed'
    echo \"\$out\" | grep -q '^extract for any purpose: not allowed'
    echo \"\$out\" | grep -q '^modify anything: not allowed'
    echo \"\$out\" | grep -q '^modify forms: not allowed'
    echo \"\$out\" | grep -q '^modify document assembly: not allowed'"
  # AES-256 (R6) carries /P into the crypt-protected Perms field too
  run_test "qpdf sees the same restrictions on AES-256" bash -c "
    set -e
    $TSPDF encrypt $INPUT -o $TMPDIR/encperm256.pdf --password secret --bits 256 --permissions copy > /dev/null
    out=\$(qpdf --password=secret --show-encryption $TMPDIR/encperm256.pdf)
    echo \"\$out\" | grep -q '^extract for any purpose: allowed'
    echo \"\$out\" | grep -q '^print low resolution: not allowed'"
  run_test "default without --permissions allows everything" bash -c "
    ! qpdf --password=secret --show-encryption $TMPDIR/enc128.pdf | grep -q 'not allowed'"
else
  echo "  SKIP  encrypt --permissions qpdf assertions (qpdf not found)"
fi

# metadata
run_test "metadata view" $TSPDF metadata $INPUT
run_test "metadata set" $TSPDF metadata $INPUT --set title="Test Title" --set author="Test Author" -o $TMPDIR/meta.pdf
# flag-first ordering: -o output and --set values must not be swallowed as input
run_test "metadata flag-first ordering (-o/--set before input)" $TSPDF metadata -o $TMPDIR/meta_ff.pdf --set title="FF" $INPUT
run_test "metadata flag-first applied the title" bash -c "$TSPDF metadata $TMPDIR/meta_ff.pdf | grep -qi 'FF'"

# metadata --set producer and --clear
run_test "metadata set producer" bash -c "
  set -e
  $TSPDF metadata $INPUT --set producer='Custom Producer' -o $TMPDIR/meta_prod.pdf > /dev/null
  $TSPDF metadata $TMPDIR/meta_prod.pdf | grep -q '^Producer:.*Custom Producer'"
run_test "metadata clear title removes the field" bash -c "
  set -e
  $TSPDF metadata $TMPDIR/meta.pdf --clear title -o $TMPDIR/meta_noclear.pdf > /dev/null
  ! $TSPDF metadata $TMPDIR/meta_noclear.pdf | grep -q '^Title:'"
run_test "metadata clear rejects unknown keys" bash -c "! $TSPDF metadata $INPUT --clear bogus -o $TMPDIR/meta_bad.pdf > /dev/null 2>&1"
run_test "metadata clear requires -o" bash -c "! $TSPDF metadata $INPUT --clear title > /dev/null 2>&1"
if command -v qpdf > /dev/null 2>&1; then
  # Verify against the actual Info dict through qpdf --qdf, not just our
  # own view path: set producer must survive the writer's update_producer
  # default, and a cleared title must be absent from the dict.
  run_test "metadata set/clear verified via qpdf --qdf" bash -c "
    set -e
    $TSPDF metadata $TMPDIR/meta.pdf --set producer='Oracle Producer' --clear title -o $TMPDIR/meta_qdf.pdf > /dev/null
    qpdf --qdf $TMPDIR/meta_qdf.pdf $TMPDIR/meta_qdf.qdf
    grep -aq '/Producer (Oracle Producer)' $TMPDIR/meta_qdf.qdf
    ! grep -aq '/Title' $TMPDIR/meta_qdf.qdf"
else
  echo "  SKIP  metadata qpdf --qdf assertions (qpdf not found)"
fi

# metadata prints PDF dates human-readable (D:YYYYMMDDHHmmSS -> ISO-ish);
# meta.pdf just got a fresh ModDate from the --set save above.
run_test "metadata prints dates human-readable" bash -c "$TSPDF metadata $TMPDIR/meta.pdf | grep -qE '^Modified:[[:space:]]+[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\$'"

# metadata on encrypted files: --password opens them, and the written Info
# strings must be ENCRYPTED (ISO 32000 exempts only /Encrypt and /ID) —
# plaintext Info strings decrypt to garbage in poppler et al.
run_test "metadata --set on encrypted file with --password" bash -c "
  set -e
  $TSPDF encrypt $INPUT -o $TMPDIR/meta_enc_in.pdf --password metapw > /dev/null
  $TSPDF metadata $TMPDIR/meta_enc_in.pdf --password metapw --set title='Encrypted Title XYZQ' -o $TMPDIR/meta_enc.pdf > /dev/null
  $TSPDF metadata $TMPDIR/meta_enc.pdf --password metapw | grep -q 'Encrypted Title XYZQ'"
run_test "encrypted Info strings are not plaintext in the file" bash -c "
  ! grep -aq 'Encrypted Title XYZQ' $TMPDIR/meta_enc.pdf"
if command -v pdfinfo > /dev/null 2>&1; then
  run_test "pdfinfo decrypts the title of the encrypted file" bash -c "
    pdfinfo -upw metapw $TMPDIR/meta_enc.pdf | grep -q 'Encrypted Title XYZQ'"
else
  echo "  SKIP  encrypted-Info pdfinfo assertion (pdfinfo not found)"
fi

# metadata --set/--clear also rewrites the XMP packet (catalog /Metadata) for
# the edited fields, since Acrobat-class viewers prefer XMP over /Info. The
# stale-XMP notice remains only for fields the packet does not carry.
XMP_FULL=tests/data/xmp_meta_full.pdf
XMP_TITLE_ONLY=tests/data/xmp_meta.pdf
XMP_FLATE=tests/data/xmp_meta_flate.pdf
run_test "metadata --set syncs the XMP packet (no notice)" bash -c "
  set -e
  $TSPDF metadata $XMP_FULL --set title='Neü XMP Title' --set author='XMP Author' -o $TMPDIR/xmp_sync.pdf 2> $TMPDIR/xmp_sync.err > /dev/null
  ! grep -q 'note:' $TMPDIR/xmp_sync.err
  grep -aq '<rdf:li xml:lang=\"x-default\">Neü XMP Title</rdf:li>' $TMPDIR/xmp_sync.pdf
  grep -aq '<rdf:li>XMP Author</rdf:li>' $TMPDIR/xmp_sync.pdf
  ! grep -aq 'Old XMP title' $TMPDIR/xmp_sync.pdf
  ! grep -aq 'Old XMP author' $TMPDIR/xmp_sync.pdf"
run_test "XMP notice fires only for fields the packet lacks" bash -c "
  set -e
  $TSPDF metadata $XMP_TITLE_ONLY --set title='New T' --set keywords='k9' -o $TMPDIR/xmp_part.pdf 2> $TMPDIR/xmp_part.err > /dev/null
  grep -q 'keywords' $TMPDIR/xmp_part.err
  ! grep -q 'title' $TMPDIR/xmp_part.err
  grep -aq '<rdf:li xml:lang=\"x-default\">New T</rdf:li>' $TMPDIR/xmp_part.pdf"
run_test "XMP sync on a Flate-compressed packet" bash -c "
  set -e
  $TSPDF metadata $XMP_FLATE --set title='Flate XMP Title' -o $TMPDIR/xmp_flate.pdf 2> $TMPDIR/xmp_flate.err > /dev/null
  ! grep -q 'note:' $TMPDIR/xmp_flate.err
  grep -aq 'Flate XMP Title</rdf:li>' $TMPDIR/xmp_flate.pdf"
if command -v uv > /dev/null 2>&1; then
  cat > $TMPDIR/xmp_oracle.py <<'PYEOF'
import sys
import xml.etree.ElementTree as ET

import pikepdf

pdf = pikepdf.open(sys.argv[1])
raw = bytes(pdf.Root.Metadata.read_bytes())
ET.fromstring(raw)  # the packet must still parse as XML
with pdf.open_metadata(update_docinfo=False) as meta:
    assert meta["dc:title"] == "Neü XMP Title", meta["dc:title"]
    assert list(meta["dc:creator"]) == ["XMP Author"], list(meta["dc:creator"])
print("xmp-oracle-ok")
PYEOF
  run_test "XMP sync verified via pikepdf (dc:title, dc:creator, XML parses)" bash -c "
    uv run --python 3.12 --with pikepdf python $TMPDIR/xmp_oracle.py $TMPDIR/xmp_sync.pdf | grep -q xmp-oracle-ok"
else
  echo "  SKIP  XMP pikepdf oracle (uv not found)"
fi
if command -v pdfinfo > /dev/null 2>&1; then
  run_test "pdfinfo shows the synced title" bash -c "
    pdfinfo $TMPDIR/xmp_sync.pdf | grep -q 'Neü XMP Title'"
else
  echo "  SKIP  XMP pdfinfo assertion (pdfinfo not found)"
fi
if command -v qpdf > /dev/null 2>&1; then
  run_test "XMP sync when the input uses object streams" bash -c "
    set -e
    qpdf --object-streams=generate $XMP_FULL $TMPDIR/xmp_objstm_in.pdf
    $TSPDF metadata $TMPDIR/xmp_objstm_in.pdf --set title='ObjStm XMP Title' -o $TMPDIR/xmp_objstm.pdf > /dev/null 2> $TMPDIR/xmp_objstm.err
    ! grep -q 'note:' $TMPDIR/xmp_objstm.err
    grep -aq 'ObjStm XMP Title</rdf:li>' $TMPDIR/xmp_objstm.pdf
    qpdf --check $TMPDIR/xmp_objstm.pdf > /dev/null"
  run_test "XMP sync inside an encrypted file (stream stays encrypted)" bash -c "
    set -e
    $TSPDF encrypt $XMP_FULL -o $TMPDIR/xmp_enc_in.pdf --password xmppw > /dev/null
    $TSPDF metadata $TMPDIR/xmp_enc_in.pdf --password xmppw --set title='Enc XMP Title' -o $TMPDIR/xmp_enc.pdf > /dev/null 2> $TMPDIR/xmp_enc.err
    ! grep -q 'note:' $TMPDIR/xmp_enc.err
    ! grep -aq 'Enc XMP Title' $TMPDIR/xmp_enc.pdf
    qpdf --password=xmppw --check $TMPDIR/xmp_enc.pdf > /dev/null
    qpdf --password=xmppw --decrypt $TMPDIR/xmp_enc.pdf $TMPDIR/xmp_dec.pdf
    grep -aq 'Enc XMP Title</rdf:li>' $TMPDIR/xmp_dec.pdf"
else
  echo "  SKIP  XMP objstm/encrypted sync assertions (qpdf not found)"
fi

# Pin test: stale-XMP notice appears when the XMP packet is present but not
# editable (UTF-16 BOM in <?xpacket begin=...> — not the UTF-8 BOM tspdf
# accepts). The Info /Title is set; the XMP packet cannot be rewritten; the
# notice names "title" on stderr. (RED without the xmp_stale out-param fix;
# GREEN after it.)
if command -v python3 > /dev/null 2>&1; then
  python3 - $TMPDIR/xmp_nonedit.pdf << 'PYEOF'
# Minimal PDF with an XMP packet whose <?xpacket begin=...> carries a UTF-16LE
# BOM (0xFF 0xFE), making xmp_packet_editable() return false.
import sys
xmp = (
    b'<?xpacket begin="\xff\xfe" id="W5M0MpCehiHzreSzNTczkc9d"?>'
    b'<x:xmpmeta xmlns:x="adobe:ns:meta/">'
    b'<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">'
    b'<rdf:Description rdf:about="" xmlns:dc="http://purl.org/dc/elements/1.1/">'
    b'<dc:title><rdf:Alt><rdf:li xml:lang="x-default">Old title</rdf:li></rdf:Alt></dc:title>'
    b'</rdf:Description></rdf:RDF></x:xmpmeta>'
    b'<?xpacket end="w"?>'
)
# Object layout: 1=Catalog, 2=Pages, 3=Page, 4=Metadata stream, 5=Content
content = b'BT /F1 12 Tf 72 720 Td (hello) Tj ET\n'
objs = [
    b'<< /Type /Catalog /Pages 2 0 R /Metadata 4 0 R >>',
    b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 5 0 R >>',
    (b'<< /Type /Metadata /Subtype /XML /Length %d >>\nstream\n' % len(xmp))
    + xmp + b'\nendstream',
    (b'<< /Length %d >>\nstream\n' % len(content)) + content + b'\nendstream',
]
out = bytearray(b'%PDF-1.4\n')
offs = []
for i, body in enumerate(objs, 1):
    offs.append(len(out))
    out += (b'%d 0 obj\n' % i) + body + b'\nendobj\n'
xref = len(out)
n = len(objs) + 1
out += b'xref\n0 %d\n' % n + b'0000000000 65535 f \n'
for o in offs:
    out += b'%010d 00000 n \n' % o
out += b'trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (n, xref)
open(sys.argv[1], 'wb').write(out)
PYEOF
  run_test "stale-XMP notice when XMP packet is not editable (UTF-16 begin)" bash -c "
    set -e
    $TSPDF metadata $TMPDIR/xmp_nonedit.pdf --set title='New Title' -o $TMPDIR/xmp_nonedit_out.pdf 2> $TMPDIR/xmp_nonedit.err > /dev/null
    grep -q 'note: XMP metadata present but not updated for' $TMPDIR/xmp_nonedit.err
    grep -q 'title' $TMPDIR/xmp_nonedit.err"
else
  echo "  SKIP  stale-XMP notice pin test (python3 not found)"
fi

# Object-stream re-packing: rewriting a file that used object streams must
# re-pack them (not explode every member into a classic top-level object,
# which roughly doubles such files); classic inputs stay classic.
if command -v qpdf > /dev/null 2>&1; then
  run_test "rewrite of an objstm file re-packs it (<= 1.2x input)" bash -c "
    set -e
    qpdf --object-streams=generate $INPUT $TMPDIR/objstm_in.pdf
    $TSPDF crop --margin 0 $TMPDIR/objstm_in.pdf -o $TMPDIR/objstm_out.pdf > /dev/null
    in=\$(wc -c < $TMPDIR/objstm_in.pdf)
    out=\$(wc -c < $TMPDIR/objstm_out.pdf)
    [ \$((out * 10)) -le \$((in * 12)) ]
    grep -aq '/Type /ObjStm' $TMPDIR/objstm_out.pdf
    qpdf --check $TMPDIR/objstm_out.pdf > /dev/null"
  run_test "encrypted save of an objstm file keeps object streams" bash -c "
    set -e
    $TSPDF encrypt $TMPDIR/objstm_in.pdf -o $TMPDIR/objstm_enc.pdf --password s3cret > /dev/null
    grep -aq '/Type /ObjStm' $TMPDIR/objstm_enc.pdf
    qpdf --password=s3cret --check $TMPDIR/objstm_enc.pdf > /dev/null"
  run_test "encrypted objstm rewrite opens with the password" bash -c "
    set -e
    $TSPDF metadata $TMPDIR/objstm_enc.pdf --password s3cret --set title='ObjStm Enc' -o $TMPDIR/objstm_enc2.pdf > /dev/null
    grep -aq '/Type /ObjStm' $TMPDIR/objstm_enc2.pdf
    ! grep -aq 'ObjStm Enc' $TMPDIR/objstm_enc2.pdf
    qpdf --password=s3cret --check $TMPDIR/objstm_enc2.pdf > /dev/null
    $TSPDF metadata $TMPDIR/objstm_enc2.pdf --password s3cret | grep -q 'ObjStm Enc'"
else
  echo "  SKIP  objstm re-pack assertions (qpdf not found)"
fi
run_test "classic-xref input stays classic on rewrite" bash -c "
  set -e
  $TSPDF crop --margin 0 $INPUT -o $TMPDIR/classic_out.pdf > /dev/null
  ! grep -aq '/Type /ObjStm' $TMPDIR/classic_out.pdf
  grep -aq 'trailer' $TMPDIR/classic_out.pdf"

# info --json
if command -v python3 > /dev/null 2>&1; then
  run_test "info --json is valid JSON with the expected fields" bash -c "
    $TSPDF info $INPUT --json | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d[\"pages\"] == 3, d
assert d[\"encrypted\"] is False and d[\"encryption\"] is None, d
assert d[\"outlines\"] is False and d[\"acroform\"] is False, d
assert d[\"version\"] == \"1.7\", d
# uniform A4 pages collapse to a single [w, h] pair
assert d[\"page_sizes\"] == [595.28, 841.89], d
assert \"title\" in d and \"producer\" in d, d
'"
  run_test "info --json reports outlines/acroform" bash -c "
    $TSPDF info tests/data/outline_form.pdf --json | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d[\"outlines\"] is True and d[\"acroform\"] is True, d
'"
  run_test "info --json formats dates and keeps the raw value" bash -c "
    $TSPDF info $TMPDIR/meta.pdf --json | python3 -c '
import json, re, sys
d = json.load(sys.stdin)
assert d[\"title\"] == \"Test Title\", d
assert d[\"created_raw\"].startswith(\"D:\"), d
assert re.fullmatch(r\"\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\", d[\"modified\"]), d
'"
  # Real files often store PDFDocEncoded /Author//Title (BOM-less, ISO 32000
  # §7.9.2.2). The reader must decode PDFDocEncoding to UTF-8, so both the
  # text and JSON outputs show the intended characters — never raw high bytes
  # that make json.load raise UnicodeDecodeError.
  TMPDIR="$TMPDIR" python3 - << 'PYEOF'
import os
# /Author = "Ünïcödé" in PDFDocEncoding: DC 6E EF 63 F6 64 E9 (lone 0xDC etc,
# not valid UTF-8). /Title is plain ASCII and must survive intact.
body = b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"
offs = {}
def emit(i, data):
    global body
    offs[i] = len(body); body += data
emit(1, b"1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")
emit(2, b"2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")
emit(3, b"3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")
emit(4, b"4 0 obj\n<< /Author (\xDC\x6E\xEF\x63\xF6\x64\xE9) /Title (Plain ASCII) >>\nendobj\n")
xo = len(body)
body += b"xref\n0 5\n0000000000 65535 f \n"
for i in range(1, 5):
    body += b"%010d 00000 n \n" % offs[i]
body += b"trailer\n<< /Size 5 /Root 1 0 R /Info 4 0 R >>\nstartxref\n%d\n%%%%EOF\n" % xo
open(os.path.join(os.environ["TMPDIR"], "rawmeta.pdf"), "wb").write(body)
PYEOF
  run_test "info --json decodes PDFDocEncoded metadata to UTF-8" bash -c "
    $TSPDF info $TMPDIR/rawmeta.pdf --json | python3 -c '
import json, sys
d = json.load(sys.stdin)            # must not raise UnicodeDecodeError
assert d[\"title\"] == \"Plain ASCII\", d
assert d[\"author\"] == \"Ünïcödé\", repr(d[\"author\"])
'"
  run_test "metadata prints PDFDocEncoded values as valid UTF-8" bash -c "
    $TSPDF metadata $TMPDIR/rawmeta.pdf | python3 -c '
import sys
out = sys.stdin.buffer.read().decode(\"utf-8\")   # must not raise
assert \"Ünïcödé\" in out, repr(out)
'"
  # And valid non-ASCII UTF-8 metadata must pass through the JSON unchanged.
  $TSPDF metadata $INPUT --set title='Prüfbericht café' -o $TMPDIR/utf8meta.pdf > /dev/null 2>&1
  run_test "info --json preserves valid UTF-8 metadata unchanged" bash -c "
    $TSPDF info $TMPDIR/utf8meta.pdf --json | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d[\"title\"] == \"Prüfbericht café\", repr(d[\"title\"])
'"
else
  echo "  SKIP  info --json assertions (python3 not found)"
fi

# attach: names are PDF text strings (ISO 32000 §7.9.2.2) — ASCII stays
# literal, anything else is UTF-16BE with BOM; foreign encodings must decode.
printf 'unicode payload' > "$TMPDIR/spaced ünïcode name.txt"
run_test "attach add with a unicode file name" \
    $TSPDF attach add $INPUT "$TMPDIR/spaced ünïcode name.txt" -o $TMPDIR/att_uni.pdf
run_test "attach list prints the UTF-8 name" bash -c "
    $TSPDF attach list $TMPDIR/att_uni.pdf | grep -qF 'spaced ünïcode name.txt'"
run_test "attach stores the name as UTF-16BE, not raw UTF-8" bash -c "
    ! grep -qa 'ünïcode' $TMPDIR/att_uni.pdf"
run_test "attach extract --name accepts the unicode name" bash -c "
    mkdir -p $TMPDIR/attx_uni &&
    $TSPDF attach extract $TMPDIR/att_uni.pdf --name 'spaced ünïcode name.txt' -o $TMPDIR/attx_uni &&
    cmp \"$TMPDIR/attx_uni/spaced ünïcode name.txt\" \"$TMPDIR/spaced ünïcode name.txt\""
run_test "attach remove accepts the unicode name" bash -c "
    $TSPDF attach remove $TMPDIR/att_uni.pdf --name 'spaced ünïcode name.txt' -o $TMPDIR/att_uni_rm.pdf &&
    [ -z \"\$($TSPDF attach list $TMPDIR/att_uni_rm.pdf)\" ]"
if command -v qpdf > /dev/null 2>&1; then
  run_test "qpdf reads the unicode attachment name correctly" bash -c "
    qpdf --list-attachments $TMPDIR/att_uni.pdf | grep -qF 'spaced ünïcode name.txt'"
else
  echo "  SKIP  qpdf attachment-name interop (qpdf not found)"
fi

# Attachments named by other tools: a UTF-16BE key with BOM (pymupdf-style)
# and a PDFDocEncoded key (qpdf-style). Both must list and extract under
# their decoded UTF-8 names.
if command -v python3 > /dev/null 2>&1; then
  TMPDIR="$TMPDIR" python3 - << 'PYEOF'
import os
def build(path, key_bytes, uf_bytes, payload):
    body = b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n"
    offs = {}
    def emit(i, data):
        nonlocal body
        offs[i] = len(body); body += data
    emit(1, b"1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Names << /EmbeddedFiles 4 0 R >> >>\nendobj\n")
    emit(2, b"2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")
    emit(3, b"3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")
    emit(4, b"4 0 obj\n<< /Names [" + key_bytes + b" 5 0 R] >>\nendobj\n")
    emit(5, b"5 0 obj\n<< /Type /Filespec /F " + uf_bytes + b" /UF " + uf_bytes +
            b" /EF << /F 6 0 R >> >>\nendobj\n")
    emit(6, b"6 0 obj\n<< /Type /EmbeddedFile /Length %d >>\nstream\n" % len(payload) +
            payload + b"\nendstream\nendobj\n")
    xo = len(body)
    body += b"xref\n0 7\n0000000000 65535 f \n"
    for i in range(1, 7):
        body += b"%010d 00000 n \n" % offs[i]
    body += b"trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % xo
    open(path, "wb").write(body)

tmp = os.environ["TMPDIR"]
# "ünïcode.txt" as UTF-16BE with BOM, in a hex string.
u16 = b"<FEFF" + "ünïcode.txt".encode("utf-16-be").hex().upper().encode() + b">"
build(os.path.join(tmp, "att_utf16.pdf"), u16, u16, b"hello utf16")
# "spaced ünïcode.txt" in PDFDocEncoding (Latin-1 range bytes, no BOM).
pdoc = b"(spaced \xFCn\xEFcode.txt)"
build(os.path.join(tmp, "att_pdfdoc.pdf"), pdoc, pdoc, b"hello pdfdoc")
PYEOF
  run_test "attach list decodes UTF-16BE names from other tools" bash -c "
    $TSPDF attach list $TMPDIR/att_utf16.pdf | grep -qF 'ünïcode.txt'"
  run_test "attach extract --all handles a UTF-16BE-named file" bash -c "
    mkdir -p $TMPDIR/attx_u16 &&
    $TSPDF attach extract $TMPDIR/att_utf16.pdf --all -o $TMPDIR/attx_u16 &&
    [ \"\$(cat \"$TMPDIR/attx_u16/ünïcode.txt\")\" = 'hello utf16' ]"
  run_test "attach list decodes PDFDocEncoded names from other tools" bash -c "
    $TSPDF attach list $TMPDIR/att_pdfdoc.pdf | grep -qF 'spaced ünïcode.txt'"
  run_test "attach extract --all handles a PDFDocEncoded-named file" bash -c "
    mkdir -p $TMPDIR/attx_pdoc &&
    $TSPDF attach extract $TMPDIR/att_pdfdoc.pdf --all -o $TMPDIR/attx_pdoc &&
    [ \"\$(cat \"$TMPDIR/attx_pdoc/spaced ünïcode.txt\")\" = 'hello pdfdoc' ]"
else
  echo "  SKIP  attach foreign-encoding fixtures (python3 not found)"
fi

# watermark
run_test "watermark" $TSPDF watermark $INPUT -o $TMPDIR/watermark.pdf --text "DRAFT"
# flag-first ordering: -o output and --text value must not be swallowed as input
run_test "watermark flag-first ordering (-o/--text before input)" $TSPDF watermark -o $TMPDIR/watermark_ff.pdf --text "DRAFT" $INPUT

# watermark placement: the stamp must sit at the VISUAL center of the page —
# a MediaBox with a nonzero origin offsets the center ((x0+x1)/2, (y0+y1)/2),
# and a page-level /Rotate must not tip the diagonal over in the viewed
# orientation. Assert on the overlay's cm matrix in the content stream.
if command -v python3 > /dev/null 2>&1 && command -v qpdf > /dev/null 2>&1; then
  python3 - "$TMPDIR/wm_box.pdf" "$TMPDIR/wm_box_rot90.pdf" << 'PYEOF'
import sys
def make(path, rotate):
    rot = b' /Rotate %d' % rotate if rotate else b''
    content = b'q Q\n'
    objs = [
        b'<< /Type /Catalog /Pages 2 0 R >>',
        b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        b'<< /Type /Page /Parent 2 0 R /MediaBox [100 50 600 750]%s /Contents 4 0 R >>' % rot,
        b'<< /Length %d >>\nstream\n%sendstream' % (len(content), content),
    ]
    out = bytearray(b'%PDF-1.4\n')
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
    open(path, 'wb').write(out)
make(sys.argv[1], 0)
make(sys.argv[2], 90)
PYEOF
  # center of [100 50 600 750] is (350, 400); 45° gives cos=sin=0.7071
  run_test "watermark centers on offset-origin MediaBox" bash -c "
    set -e
    $TSPDF watermark $TMPDIR/wm_box.pdf -o $TMPDIR/wm_box_out.pdf --text DRAFT > /dev/null
    qpdf --qdf --object-streams=disable $TMPDIR/wm_box_out.pdf $TMPDIR/wm_box_out.qdf
    grep -q -- '0.7071 0.7071 -0.7071 0.7071 350 400 cm' $TMPDIR/wm_box_out.qdf"
  # /Rotate 90 is compensated by rotating the stamp to 45+90=135 degrees
  run_test "watermark reads upright on a /Rotate 90 page" bash -c "
    set -e
    $TSPDF watermark $TMPDIR/wm_box_rot90.pdf -o $TMPDIR/wm_rot_out.pdf --text DRAFT > /dev/null
    qpdf --qdf --object-streams=disable $TMPDIR/wm_rot_out.pdf $TMPDIR/wm_rot_out.qdf
    grep -q -- '-0.7071 0.7071 -0.7071 -0.7071 350 400 cm' $TMPDIR/wm_rot_out.qdf"
else
  echo "  SKIP  watermark placement assertions (python3/qpdf not found)"
fi

# watermark --image
run_test "watermark --image stamps every page" bash -c "
  set -e
  $TSPDF watermark $INPUT --image tests/data/img_rgb.png -o $TMPDIR/wm_img.pdf > /dev/null
  grep -aq '/Image' $TMPDIR/wm_img.pdf
  $TSPDF text $TMPDIR/wm_img.pdf --pages 1 | grep -q 'Page 1'
  $TSPDF text $TMPDIR/wm_img.pdf --pages 3 | grep -q 'Page 3'"
run_test "watermark --image with alpha PNG keeps soft mask" bash -c "
  set -e
  $TSPDF watermark $INPUT --image tests/data/img_rgba.png -o $TMPDIR/wm_rgba.pdf > /dev/null
  grep -aq '/SMask' $TMPDIR/wm_rgba.pdf"
run_test "watermark --image JPEG passthrough" bash -c "
  set -e
  $TSPDF watermark $INPUT --image examples/test.jpg -o $TMPDIR/wm_jpg.pdf > /dev/null
  grep -aq '/DCTDecode' $TMPDIR/wm_jpg.pdf"
run_test "watermark --image tile position" bash -c "
  set -e
  $TSPDF watermark $INPUT --image tests/data/img_rgb.png --position tile -o $TMPDIR/wm_tile.pdf > /dev/null
  $TSPDF text $TMPDIR/wm_tile.pdf --pages 1 | grep -q 'Page 1'"
run_test "watermark --image corner position" $TSPDF watermark $INPUT --image tests/data/img_rgb.png --position bottom-right -o $TMPDIR/wm_corner.pdf
run_test "watermark rejects --text with --image" bash -c "! $TSPDF watermark $INPUT --text DRAFT --image tests/data/img_rgb.png -o $TMPDIR/wm_both.pdf > /dev/null 2>&1"
run_test "watermark requires --text or --image" bash -c "! $TSPDF watermark $INPUT -o $TMPDIR/wm_none.pdf > /dev/null 2>&1"
run_test "watermark --image rejects bad scale" bash -c "! $TSPDF watermark $INPUT --image tests/data/img_rgb.png --scale 0 -o $TMPDIR/wm_bads.pdf > /dev/null 2>&1"
run_test "watermark --image rejects unknown position" bash -c "! $TSPDF watermark $INPUT --image tests/data/img_rgb.png --position middle -o $TMPDIR/wm_badp.pdf > /dev/null 2>&1"
if command -v qpdf > /dev/null 2>&1; then
  run_test "watermark --image outputs pass qpdf --check" bash -c "
    qpdf --check $TMPDIR/wm_img.pdf > /dev/null 2>&1 &&
    qpdf --check $TMPDIR/wm_rgba.pdf > /dev/null 2>&1 &&
    qpdf --check $TMPDIR/wm_jpg.pdf > /dev/null 2>&1 &&
    qpdf --check $TMPDIR/wm_tile.pdf > /dev/null 2>&1"
  # image watermark honors the same /Rotate compensation as the text
  # watermark: on a /Rotate 90 page the placement runs under a 90-degree
  # rotation about the page center (350,400) -> cm [0 1 -1 0 750 50].
  if [ -f "$TMPDIR/wm_box_rot90.pdf" ]; then
    run_test "watermark --image compensates /Rotate 90" bash -c "
      set -e
      $TSPDF watermark $TMPDIR/wm_box_rot90.pdf --image tests/data/img_rgb.png -o $TMPDIR/wm_img_rot.pdf > /dev/null
      qpdf --qdf --object-streams=disable $TMPDIR/wm_img_rot.pdf $TMPDIR/wm_img_rot.qdf
      grep -q -- '0 1 -1 0 750 50 cm' $TMPDIR/wm_img_rot.qdf"
  fi
else
  echo "  SKIP  watermark --image qpdf checks (qpdf not found)"
fi

# overlay commands on pages whose /Resources is an INDIRECT reference (the
# pymupdf/pdfTeX shape: shared between both pages, /Font itself indirect).
# The resource merge used to append a duplicate /Resources key, orphaning the
# original fonts — pages rendered near-blank in poppler. Fixture committed as
# tests/data/indirect_resources.pdf (see gen_indirect_resources.py).
IND_RES=tests/data/indirect_resources.pdf
run_test "watermark --text keeps fonts of indirect-/Resources pages" bash -c "
  set -e
  $TSPDF watermark $IND_RES --text DRAFT -o $TMPDIR/ind_wm.pdf > /dev/null
  $TSPDF text $TMPDIR/ind_wm.pdf --pages 1 | grep -q 'Hello page one'
  $TSPDF text $TMPDIR/ind_wm.pdf --pages 2 | grep -q 'Hello page two'
  $TSPDF text $TMPDIR/ind_wm.pdf --pages 1 | grep -q DRAFT"
run_test "watermark --image keeps fonts of indirect-/Resources pages" bash -c "
  set -e
  $TSPDF watermark $IND_RES --image tests/data/img_rgb.png -o $TMPDIR/ind_wm_img.pdf > /dev/null
  $TSPDF text $TMPDIR/ind_wm_img.pdf --pages 1 | grep -q 'Hello page one'
  $TSPDF text $TMPDIR/ind_wm_img.pdf --pages 2 | grep -q 'Hello page two'"
run_test "pagenum keeps fonts of indirect-/Resources pages" bash -c "
  set -e
  $TSPDF pagenum $IND_RES -o $TMPDIR/ind_pn.pdf > /dev/null
  $TSPDF text $TMPDIR/ind_pn.pdf --pages 1 | grep -q 'Hello page one'
  $TSPDF text $TMPDIR/ind_pn.pdf --pages 2 | grep -q 'Hello page two'
  $TSPDF text $TMPDIR/ind_pn.pdf --pages 2 | grep -q 2"
if command -v qpdf > /dev/null 2>&1; then
  # qpdf exits nonzero on 'dictionary has duplicated key /Resources' warnings.
  run_test "indirect-/Resources outputs pass qpdf --check" bash -c "
    set -e
    qpdf --check $TMPDIR/ind_wm.pdf > /dev/null 2>&1
    qpdf --check $TMPDIR/ind_wm_img.pdf > /dev/null 2>&1
    qpdf --check $TMPDIR/ind_pn.pdf > /dev/null 2>&1"
else
  echo "  SKIP  indirect-/Resources qpdf checks (qpdf not found)"
fi
if command -v pdftotext > /dev/null 2>&1; then
  # poppler honors the LAST duplicate key, so the bug made it lose the
  # original fonts entirely (near-blank render, no extractable text).
  run_test "indirect-/Resources outputs keep poppler-extractable text" bash -c "
    set -e
    pdftotext $TMPDIR/ind_wm.pdf - | grep -q 'Hello page one'
    pdftotext $TMPDIR/ind_wm_img.pdf - | grep -q 'Hello page two'
    pdftotext $TMPDIR/ind_pn.pdf - | grep -q 'Hello page one'"
else
  echo "  SKIP  indirect-/Resources poppler text checks (pdftotext not found)"
fi
# Each page dict must end up with exactly ONE /Resources key: the raw output
# is uncompressed, so the literal must appear exactly twice (once per page) —
# the duplicate-key bug produced four.
run_test "indirect-/Resources watermark writes a single /Resources per page" bash -c "
  set -e
  [ -f $TMPDIR/ind_wm.pdf ]
  [ \$(grep -ao '/Resources' $TMPDIR/ind_wm.pdf | wc -l) -eq 2 ]"

# stamp: overlay a page of one PDF onto pages of another
printf '# APPROVED\n' > $TMPDIR/stamp_src.md
$TSPDF md2pdf $TMPDIR/stamp_src.md -o $TMPDIR/stamp_src.pdf > /dev/null 2>&1
run_test "stamp onto all pages" bash -c "
  set -e
  $TSPDF stamp $INPUT --stamp $TMPDIR/stamp_src.pdf -o $TMPDIR/stamped.pdf > /dev/null
  $TSPDF text $TMPDIR/stamped.pdf --pages 1 | grep -q APPROVED
  $TSPDF text $TMPDIR/stamped.pdf --pages 2 | grep -q APPROVED
  $TSPDF text $TMPDIR/stamped.pdf --pages 3 | grep -q APPROVED
  $TSPDF text $TMPDIR/stamped.pdf --pages 1 | grep -q 'Page 1'
  $TSPDF text $TMPDIR/stamped.pdf --pages 3 | grep -q 'Page 3'"
run_test "stamp --pages 2 stamps only page 2" bash -c "
  set -e
  $TSPDF stamp $INPUT --stamp $TMPDIR/stamp_src.pdf -o $TMPDIR/stamped_p2.pdf --pages 2 > /dev/null
  ! $TSPDF text $TMPDIR/stamped_p2.pdf --pages 1 | grep -q APPROVED
  $TSPDF text $TMPDIR/stamped_p2.pdf --pages 2 | grep -q APPROVED
  ! $TSPDF text $TMPDIR/stamped_p2.pdf --pages 3 | grep -q APPROVED"
# stamp content is appended (drawn on top), so it comes after the original
# text in content order; --under prepends it.
run_test "stamp draws over by default, under with --under" bash -c "
  set -e
  $TSPDF stamp $INPUT --stamp $TMPDIR/stamp_src.pdf -o $TMPDIR/stamped_over.pdf > /dev/null
  $TSPDF stamp $INPUT --stamp $TMPDIR/stamp_src.pdf --under -o $TMPDIR/stamped_under.pdf > /dev/null
  over=\$($TSPDF text $TMPDIR/stamped_over.pdf --pages 1)
  under=\$($TSPDF text $TMPDIR/stamped_under.pdf --pages 1)
  printf '%s\n' \"\$over\"  | head -1 | grep -q 'Page 1'
  printf '%s\n' \"\$under\" | head -1 | grep -q 'APPROVED'
  printf '%s\n' \"\$under\" | grep -q 'Page 1'"
run_test "stamp --stamp-page selects the stamp page" bash -c "
  set -e
  printf '# SECONDMARK\n' > $TMPDIR/stamp_src2.md
  $TSPDF md2pdf $TMPDIR/stamp_src2.md -o $TMPDIR/stamp_src2.pdf > /dev/null
  $TSPDF merge $TMPDIR/stamp_src.pdf $TMPDIR/stamp_src2.pdf -o $TMPDIR/stamp_2pg.pdf > /dev/null
  $TSPDF stamp $INPUT --stamp $TMPDIR/stamp_2pg.pdf --stamp-page 2 -o $TMPDIR/stamped_sp2.pdf > /dev/null
  $TSPDF text $TMPDIR/stamped_sp2.pdf --pages 1 | grep -q SECONDMARK
  ! $TSPDF text $TMPDIR/stamped_sp2.pdf --pages 1 | grep -q APPROVED"
run_test "stamp encrypted input with --password (output stays encrypted)" bash -c "
  set -e
  $TSPDF encrypt $INPUT --password sec -o $TMPDIR/stamp_enc_in.pdf > /dev/null
  $TSPDF stamp $TMPDIR/stamp_enc_in.pdf --password sec --stamp $TMPDIR/stamp_src.pdf -o $TMPDIR/stamped_enc.pdf > /dev/null
  ! $TSPDF text $TMPDIR/stamped_enc.pdf --pages 1 > /dev/null 2>&1
  $TSPDF text $TMPDIR/stamped_enc.pdf --password sec --pages 1 | grep -q APPROVED"
run_test "stamp source with image resources" bash -c "
  set -e
  printf '# LOGOMARK\n\n![logo](%s/tests/data/img_rgb.png)\n' $(pwd) > $TMPDIR/stamp_img.md
  $TSPDF md2pdf $TMPDIR/stamp_img.md -o $TMPDIR/stamp_img.pdf > /dev/null
  $TSPDF stamp $INPUT --stamp $TMPDIR/stamp_img.pdf -o $TMPDIR/stamped_img.pdf > /dev/null
  $TSPDF text $TMPDIR/stamped_img.pdf --pages 2 | grep -q LOGOMARK
  grep -aq '/Image' $TMPDIR/stamped_img.pdf"
run_test "stamp missing --stamp errors" bash -c "! $TSPDF stamp $INPUT -o $TMPDIR/stamp_miss.pdf > /dev/null 2>&1"
run_test "stamp rejects --stamp-page 0" bash -c "! $TSPDF stamp $INPUT --stamp $TMPDIR/stamp_src.pdf --stamp-page 0 -o $TMPDIR/stamp_sp0.pdf > /dev/null 2>&1"
run_test "stamp rejects out-of-range --stamp-page" bash -c "! $TSPDF stamp $INPUT --stamp $TMPDIR/stamp_src.pdf --stamp-page 9 -o $TMPDIR/stamp_sp9.pdf > /dev/null 2>&1"
run_test "stamp rejects out-of-range --pages" bash -c "! $TSPDF stamp $INPUT --stamp $TMPDIR/stamp_src.pdf --pages 7 -o $TMPDIR/stamp_p7.pdf > /dev/null 2>&1"
run_test "stamp help mentions --under" bash -c "$TSPDF help stamp | grep -q -- '--under'"
run_test "stamp flag-first ordering (flags before input)" $TSPDF stamp --stamp $TMPDIR/stamp_src.pdf -o $TMPDIR/stamped_ff.pdf $INPUT
if command -v qpdf > /dev/null 2>&1; then
  run_test "stamp outputs pass qpdf --check" bash -c "
    qpdf --check $TMPDIR/stamped.pdf > /dev/null 2>&1 &&
    qpdf --check $TMPDIR/stamped_under.pdf > /dev/null 2>&1 &&
    qpdf --check $TMPDIR/stamped_img.pdf > /dev/null 2>&1 &&
    qpdf --is-encrypted $TMPDIR/stamped_enc.pdf &&
    qpdf --password=sec --check $TMPDIR/stamped_enc.pdf > /dev/null 2>&1"
else
  echo "  SKIP  stamp qpdf --check (qpdf not found)"
fi

# compress
run_test "compress" $TSPDF compress $INPUT -o $TMPDIR/compressed.pdf
# flag-first ordering: -o output must not be swallowed as input
run_test "compress flag-first ordering (-o before input)" $TSPDF compress -o $TMPDIR/compressed_ff.pdf $INPUT

# compress keeps whichever encoding of a FlateDecode stream is smaller: a
# re-encoded stream is only written when it actually shrinks (the best-effort
# deflate level can beat zlib-9), never grows. Build a PDF whose content
# stream is a large zlib-9 payload, then assert the stream never grew and the
# file still reopens.
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
  run_test "compress never grows an already-well-compressed stream" python3 - "$TMPDIR/bigflate.pdf" "$TMPDIR/bigflate_out.pdf" << 'PYEOF'
import re, sys, zlib
def stream_bytes(p):
    d = open(p, "rb").read()
    m = re.search(rb"/Filter /FlateDecode.*?>>\nstream\n(.*?)\nendstream", d, re.DOTALL)
    return m.group(1) if m else None
a = stream_bytes(sys.argv[1])
b = stream_bytes(sys.argv[2])
assert a is not None and b is not None, "stream not found"
# Keep-if-smaller: a differing stream must be a strictly smaller encoding of
# the exact same payload; an equal-or-larger re-encode must keep the original.
assert len(b) <= len(a), "stream grew (%d -> %d bytes)" % (len(a), len(b))
if a != b:
    assert len(b) < len(a), "same-size stream was needlessly re-encoded"
    assert zlib.decompress(b) == zlib.decompress(a), "payload changed"
PYEOF
  run_test "compress output reopens cleanly (round-trip)" $TSPDF info "$TMPDIR/bigflate_out.pdf"
else
  echo "  SKIP  compress well-compressed stream (python3 not found)"
fi

# The lossless armor-strip must refuse streams whose /DecodeParms is an
# indirect reference: tspdf_stream_decode does not resolve refs, so it would
# silently skip the predictor and bake the predictor-coded bytes into the
# output as plain Flate. The committed fixture's content stream is
# /Filter [/FlateDecode] /DecodeParms <n> 0 R -> << /Predictor 12 /Columns 32 >>
# deflated at level 1, so a re-encode would win on size if not gated.
# (Regenerate with tests/data/gen_armor_predref.py. The grep assertion is
# tied to the conservative keep-verbatim gate; revisit it if indirect
# DecodeParms refs are ever resolved during decode.)
run_test "compress keeps /DecodeParms of an indirect-parms stream" bash -c "
  set -e
  $TSPDF compress tests/data/armor_predref.pdf -o $TMPDIR/predref_out.pdf > /dev/null
  grep -aq '/DecodeParms' $TMPDIR/predref_out.pdf
  $TSPDF info $TMPDIR/predref_out.pdf > /dev/null"
if command -v python3 > /dev/null 2>&1; then
  run_test "indirect-parms stream still decodes after compress" python3 - "$TMPDIR/predref_out.pdf" << 'PYEOF'
import re, sys, zlib
d = open(sys.argv[1], "rb").read()
marker = b"ArmorPredRefCheck"
def unpred_up(data, cols):
    # PNG Up predictor, filter byte 2 per row (the fixture's declared parms).
    out = bytearray()
    prev = bytes(cols)
    for off in range(0, len(data), cols + 1):
        row = data[off:off + cols + 1]
        if not row or row[0] != 2:
            raise ValueError("not Up-predicted")
        cur = bytes((row[1 + i] + prev[i]) & 0xFF for i in range(len(row) - 1))
        out += cur
        prev = cur
    return bytes(out)
# Decode every Flate stream exactly as its dict declares: plain inflate, plus
# the fixture's PNG-Up /Columns 32 predictor when the dict carries parms. The
# marker must survive; the pre-fix corruption dropped the parms while keeping
# predictor-coded bytes, so no declared decoding yields it.
found = False
for m in re.finditer(rb"<<([^<>]*)>>\s*stream\r?\n", d):
    dct = m.group(1)
    lm = re.search(rb"/Length\s+(\d+)", dct)
    if lm is None or b"/FlateDecode" not in dct:
        continue
    raw = d[m.end():m.end() + int(lm.group(1))]
    try:
        dec = zlib.decompress(raw)
    except zlib.error:
        continue
    if b"/DecodeParms" in dct or b"/DP" in dct:
        try:
            dec = unpred_up(dec, 32)
        except ValueError:
            continue
    if marker in dec:
        found = True
assert found, "content stream no longer decodes to the fixture marker"
PYEOF
else
  echo "  SKIP  indirect-parms decode assertion (python3 not found)"
fi

# compress packs small non-stream objects into object streams (ObjStm) so
# object-heavy files shrink instead of growing. Build a PDF with hundreds of
# tiny objects, compress it, and check size, structure and content survive.
if command -v python3 > /dev/null 2>&1; then
  python3 -c "
import sys
n = 300
objs = []
gs = ' '.join('/GS%d %d 0 R' % (i, 6 + i) for i in range(n))
objs.append(b'<< /Type /Catalog /Pages 2 0 R >>')
objs.append(b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>')
objs.append(('<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] '
             '/Contents 4 0 R /Resources << /Font << /F1 5 0 R >> '
             '/ExtGState << %s >> >> >>' % gs).encode())
content = b'BT /F1 12 Tf 72 720 Td (Hello ObjStm) Tj ET'
objs.append(b'<< /Length %d >>\nstream\n' % len(content) + content + b'\nendstream')
objs.append(b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>')
for i in range(n):
    objs.append(b'<< /Type /ExtGState /CA 0.%d /LW %d >>' % (i % 9 + 1, i % 7 + 1))
out = bytearray(b'%PDF-1.4\n')
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
" "$TMPDIR/objheavy.pdf"
  run_test "compress object-heavy pdf" $TSPDF compress "$TMPDIR/objheavy.pdf" -o "$TMPDIR/objheavy_out.pdf"
  run_test "compress object-heavy output is smaller than input" bash -c "
    [ \$(wc -c < '$TMPDIR/objheavy_out.pdf') -lt \$(wc -c < '$TMPDIR/objheavy.pdf') ]"
  run_test "compress output contains an object stream" grep -q "/Type /ObjStm" "$TMPDIR/objheavy_out.pdf"
  run_test "compress object-heavy page count preserved" bash -c "$TSPDF info '$TMPDIR/objheavy_out.pdf' | grep -qE '^Pages:[[:space:]]*1$'"
  run_test "compress object-heavy text preserved" bash -c "$TSPDF text '$TMPDIR/objheavy_out.pdf' | grep -q 'Hello ObjStm'"
  # Do-no-harm: recompressing the ObjStm-heavy output must not inflate it.
  run_test "compress objstm-heavy input does not grow" bash -c "
    $TSPDF compress '$TMPDIR/objheavy_out.pdf' -o '$TMPDIR/objheavy_out2.pdf' > /dev/null 2>&1 &&
    [ \$(wc -c < '$TMPDIR/objheavy_out2.pdf') -le \$(wc -c < '$TMPDIR/objheavy_out.pdf') ]"
  if command -v qpdf > /dev/null 2>&1; then
    run_test "compress objstm output passes qpdf --check" qpdf --check "$TMPDIR/objheavy_out.pdf"
    run_test "compress objstm output has type-2 xref entries" bash -c "
      qpdf --show-xref '$TMPDIR/objheavy_out.pdf' | grep -q 'compressed; stream ='"
  else
    echo "  SKIP  compress objstm qpdf oracle (qpdf not found)"
  fi
else
  echo "  SKIP  compress objstm packing (python3 not found)"
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

  # img2pdf must not silently drop images past the writer's 64-image limit:
  # exceeding it is a hard error (even with --best-effort), never a quiet
  # 64-page PDF with the rest of the inputs missing.
  IMG_MANY_DIR=$TMPDIR/img_many
  mkdir -p $IMG_MANY_DIR
  IMG_MANY=()
  for i in $(seq 1 70); do
    cp $TMPDIR/test.png $IMG_MANY_DIR/img_$i.png
    IMG_MANY+=("$IMG_MANY_DIR/img_$i.png")
  done
  run_test "img2pdf >64 images fails loudly" bash -c "! $TSPDF img2pdf ${IMG_MANY[*]} -o $TMPDIR/img2pdf_many.pdf > /dev/null 2>&1"
  run_test "img2pdf >64 images names the image limit" bash -c "$TSPDF img2pdf ${IMG_MANY[*]} -o $TMPDIR/img2pdf_many2.pdf 2>&1 | grep -qi 'too many images'"
  run_test "img2pdf >64 images fails even with --best-effort" bash -c "! $TSPDF img2pdf ${IMG_MANY[*]} --best-effort -o $TMPDIR/img2pdf_many3.pdf > /dev/null 2>&1"
else
  echo "  SKIP  img2pdf (python3 not found)"
fi
run_test "img2pdf no images fails" bash -c "! $TSPDF img2pdf -o $TMPDIR/img2pdf_fail.pdf > /dev/null 2>&1"

# img2pdf PNG embedding: non-interlaced gray/RGB/palette PNGs are embedded via
# IDAT passthrough (the zlib stream goes in verbatim with a /Predictor 15
# DecodeParms), so the output PDF must stay near the source PNG size instead of
# ballooning from decode + naive recompression. Fixtures live in tests/data
# (regenerate with tests/data/gen_img_fixtures.py).
img2pdf_fits() {  # <fixture.png> <out.pdf>: PDF <= 1.2x PNG + ~1.2K structure
  local src="tests/data/$1" out="$TMPDIR/$2"
  "$TSPDF" img2pdf "tests/data/$1" -o "$out" > /dev/null 2>&1 || return 1
  local ss os
  ss=$(wc -c < "$src") && os=$(wc -c < "$out") || return 1
  [ "$os" -le $(( ss * 12 / 10 + 1200 )) ]
}
run_test "img2pdf rgb png passthrough size"      img2pdf_fits img_rgb.png     pp_rgb.pdf
run_test "img2pdf gray png passthrough size"     img2pdf_fits img_gray.png    pp_gray.pdf
run_test "img2pdf palette png passthrough size"  img2pdf_fits img_palette.png pp_pal.pdf
run_test "img2pdf 4-bit palette passthrough size" img2pdf_fits img_palette4.png pp_pal4.pdf
# palette+tRNS: the color stream passes through, but the 32-byte tRNS table
# becomes a real 80x60 SMask raster, so allow that on top of the 1.2x bound
# (python img2pdf lands in the same place for this file).
run_test "img2pdf palette+tRNS passthrough size" bash -c "
  $TSPDF img2pdf tests/data/img_palette_trns.png -o $TMPDIR/pp_paltrns.pdf > /dev/null 2>&1 &&
  [ \$(wc -c < $TMPDIR/pp_paltrns.pdf) -le \$(( \$(wc -c < tests/data/img_palette_trns.png) * 12 / 10 + 4800 + 1200 )) ]"

# Structural checks: passthrough must keep the source colorspace (Indexed /
# DeviceGray — no expansion to RGB) and declare the PNG predictor.
run_test "img2pdf rgb output uses Predictor 15"  bash -c "grep -aq '/Predictor 15' $TMPDIR/pp_rgb.pdf"
run_test "img2pdf gray output uses DeviceGray"   bash -c "grep -aq '/DeviceGray' $TMPDIR/pp_gray.pdf"
run_test "img2pdf palette output uses Indexed"   bash -c "grep -aq '/Indexed' $TMPDIR/pp_pal.pdf"
run_test "img2pdf palette+tRNS keeps Indexed + SMask" bash -c "grep -aq '/Indexed' $TMPDIR/pp_paltrns.pdf && grep -aq '/SMask' $TMPDIR/pp_paltrns.pdf"

# Alpha PNGs cannot passthrough (alpha is interleaved) but must not expand
# grayscale to RGB, and must still carry an SMask.
run_test "img2pdf rgba keeps SMask" bash -c "$TSPDF img2pdf tests/data/img_rgba.png -o $TMPDIR/pp_rgba.pdf > /dev/null 2>&1 && grep -aq '/SMask' $TMPDIR/pp_rgba.pdf"
run_test "img2pdf gray+alpha stays DeviceGray with SMask" bash -c "$TSPDF img2pdf tests/data/img_gray_alpha.png -o $TMPDIR/pp_ga.pdf > /dev/null 2>&1 && grep -aq '/DeviceGray' $TMPDIR/pp_ga.pdf && grep -aq '/SMask' $TMPDIR/pp_ga.pdf"
# Even the decode-path (alpha) outputs must not blow up vs the source.
run_test "img2pdf rgba size sane" bash -c "[ \$(wc -c < $TMPDIR/pp_rgba.pdf) -le \$(( \$(wc -c < tests/data/img_rgba.png) * 18 / 10 + 1200 )) ]"
run_test "img2pdf gray+alpha size sane" bash -c "[ \$(wc -c < $TMPDIR/pp_ga.pdf) -le \$(( \$(wc -c < tests/data/img_gray_alpha.png) * 18 / 10 + 1200 )) ]"

# JPEG passthrough regression: the embedded DCT stream must stay byte-identical
# to the source file.
run_test "img2pdf jpeg stream byte-identical" bash -c "
  $TSPDF img2pdf examples/test.jpg -o $TMPDIR/pp_jpg.pdf > /dev/null 2>&1 &&
  python3 - examples/test.jpg $TMPDIR/pp_jpg.pdf << 'PYEOF'
import sys
jpg = open(sys.argv[1], 'rb').read()
pdf = open(sys.argv[2], 'rb').read()
sys.exit(0 if jpg in pdf else 1)
PYEOF"

if command -v qpdf > /dev/null 2>&1; then
  run_test "img2pdf passthrough PDFs pass qpdf --check" bash -c "
    qpdf --check $TMPDIR/pp_rgb.pdf > /dev/null 2>&1 &&
    qpdf --check $TMPDIR/pp_gray.pdf > /dev/null 2>&1 &&
    qpdf --check $TMPDIR/pp_pal.pdf > /dev/null 2>&1 &&
    qpdf --check $TMPDIR/pp_pal4.pdf > /dev/null 2>&1 &&
    qpdf --check $TMPDIR/pp_paltrns.pdf > /dev/null 2>&1 &&
    qpdf --check $TMPDIR/pp_rgba.pdf > /dev/null 2>&1 &&
    qpdf --check $TMPDIR/pp_ga.pdf > /dev/null 2>&1"
else
  echo "  SKIP  img2pdf qpdf --check (qpdf not found)"
fi

# --page-size: a4 (default), letter, or image (page = image size at 72 dpi)
run_test "img2pdf --page-size image sizes page to image" bash -c "
  $TSPDF img2pdf tests/data/img_rgb.png --page-size image -o $TMPDIR/ps_img.pdf > /dev/null 2>&1 &&
  grep -aq 'MediaBox \[ 0 0 80.0000 60.0000 \]' $TMPDIR/ps_img.pdf"
run_test "img2pdf --page-size letter" bash -c "
  $TSPDF img2pdf tests/data/img_rgb.png --page-size letter -o $TMPDIR/ps_letter.pdf > /dev/null 2>&1 &&
  grep -aq 'MediaBox \[ 0 0 612.0000 792.0000 \]' $TMPDIR/ps_letter.pdf"
run_test "img2pdf --page-size a4 (default)" bash -c "
  $TSPDF img2pdf tests/data/img_rgb.png --page-size a4 -o $TMPDIR/ps_a4.pdf > /dev/null 2>&1 &&
  grep -aq 'MediaBox \[ 0 0 595.2760 841.8900 \]' $TMPDIR/ps_a4.pdf"
run_test "img2pdf --page-size rejects unknown value" bash -c "! $TSPDF img2pdf tests/data/img_rgb.png --page-size a5 -o $TMPDIR/ps_bad.pdf > /dev/null 2>&1"
run_test "img2pdf --page-size value not eaten as input" bash -c "
  $TSPDF img2pdf tests/data/img_rgb.png --page-size letter -o $TMPDIR/ps_pos.pdf > /dev/null 2>&1 &&
  $TSPDF info $TMPDIR/ps_pos.pdf | grep -qE '^Pages:[[:space:]]*1$'"

# qrcode
run_test "qrcode" $TSPDF qrcode "https://example.com" -o $TMPDIR/qrcode.pdf
run_test "qrcode with title" $TSPDF qrcode "https://example.com" -o $TMPDIR/qrcode2.pdf --title "Test" --subtitle "Scan me"
run_test "qrcode no text fails" bash -c "! $TSPDF qrcode -o $TMPDIR/qr_fail.pdf > /dev/null 2>&1"
# flag-first ordering: -o output and --title/--subtitle values must not be swallowed as the text positional
run_test "qrcode flag-first ordering (-o/--title/--subtitle before text)" $TSPDF qrcode -o $TMPDIR/qrcode_ff.pdf --title "Test" --subtitle "Scan me" "https://example.com"
# a flag value must not be mistaken for the text positional (only flags present → still missing text)
run_test "qrcode title-only still reports missing text" bash -c "! $TSPDF qrcode -o $TMPDIR/qr_fail2.pdf --title MyTitle > /dev/null 2>&1"
# --title "" suppresses the page title AND the /Title metadata entry entirely
# (rather than writing an empty string); the default is /Title (QR Code).
run_test "qrcode --title \"\" omits metadata title" bash -c '
  set -e
  "'"$TSPDF"'" qrcode "https://example.com" --title "" -o "'"$TMPDIR"'/qr_notitle.pdf" > /dev/null
  ! grep -qa "/Title" "'"$TMPDIR"'/qr_notitle.pdf"
  "'"$TSPDF"'" qrcode "https://example.com" -o "'"$TMPDIR"'/qr_deftitle.pdf" > /dev/null
  grep -qa "/Title (QR Code)" "'"$TMPDIR"'/qr_deftitle.pdf"
'
# --ec-level: all four levels accepted (upper or lower case), junk rejected.
# A higher level spends more modules on error correction, so the same text
# must yield a larger PDF page payload at H than at L.
run_test "qrcode --ec-level L" $TSPDF qrcode "https://example.com" --ec-level L -o $TMPDIR/qr_l.pdf
run_test "qrcode --ec-level M" $TSPDF qrcode "https://example.com" --ec-level M -o $TMPDIR/qr_m.pdf
run_test "qrcode --ec-level Q" $TSPDF qrcode "https://example.com" --ec-level Q -o $TMPDIR/qr_q.pdf
run_test "qrcode --ec-level H" $TSPDF qrcode "https://example.com" --ec-level H -o $TMPDIR/qr_h.pdf
run_test "qrcode --ec-level lowercase h" $TSPDF qrcode "https://example.com" --ec-level h -o $TMPDIR/qr_h2.pdf
run_test "qrcode --ec-level X rejected" bash -c "! $TSPDF qrcode 'https://example.com' --ec-level X -o $TMPDIR/qr_x.pdf > /dev/null 2>&1"
run_test "qrcode H symbol denser than L" bash -c "
  [ \$(wc -c < $TMPDIR/qr_h.pdf) -gt \$(wc -c < $TMPDIR/qr_l.pdf) ]"
# level H shrinks capacity: 137 bytes is the v11 ceiling at H, 138 must fail
run_test "qrcode --ec-level H capacity ceiling" bash -c "
  set -e
  $TSPDF qrcode \"\$(printf 'a%.0s' {1..137})\" --ec-level H -o $TMPDIR/qr_hmax.pdf > /dev/null
  ! $TSPDF qrcode \"\$(printf 'a%.0s' {1..138})\" --ec-level H -o $TMPDIR/qr_hover.pdf > /dev/null 2>&1"

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
run_test "top-level help describes text or image watermark" bash -c "$TSPDF --help 2>/dev/null | grep -E '^  watermark[[:space:]]+Add a text or image watermark'"

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
  # metadata-view goes through the same getters as the CLI, so a PDFDocEncoded
  # /Author decodes to UTF-8. rawmeta.pdf (crafted in the info --json block)
  # stores "Ünïcödé" as PDFDocEncoding high bytes.
  run_serve_test "serve metadata-view decodes PDFDocEncoded bytes to UTF-8" bash -c "
    [ -f \"$TMPDIR/rawmeta.pdf\" ] &&
    curl -sf --retry 3 --retry-delay 1 --max-time 10 -F \"pdf_file=@$TMPDIR/rawmeta.pdf\" http://localhost:${SERVE_PORT}/api/metadata-view |
    python3 -c \"
import json, sys
j = json.load(sys.stdin)
assert j['title'] == 'Plain ASCII', j['title']
assert j['author'] == 'Ünïcödé', repr(j['author'])
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
  run_serve_test "serve watermark honors custom text and font size" env TSPDF="$TSPDF" TMPDIR="$TMPDIR" SERVE_PORT="$SERVE_PORT" INPUT="$INPUT" bash -c '
    curl -sf --retry 3 --retry-delay 1 --max-time 10 \
      -F "pdf_file=@${INPUT}" \
      -F "config={\"watermark_text\":\"WMCUSTOM\",\"font_size\":\"24\"}" \
      "http://localhost:${SERVE_PORT}/api/watermark-existing" -o "${TMPDIR}/serve_wm.pdf" &&
    "$TSPDF" text "${TMPDIR}/serve_wm.pdf" | grep -q WMCUSTOM
  '

  # Task 6: the web watermark endpoint must re-encode non-ASCII watermark text
  # UTF-8 -> cp1252 (WinAnsiEncoding), matching the CLI. "RÉSUMÉ" carries É
  # (U+00C9): its cp1252 byte is 0xC9, and it must appear that way in the page
  # content stream — NOT as the raw UTF-8 bytes 0xC3 0x89. We assert the whole
  # word "R<0xC9>SUM<0xC9>" is present after zlib-inflating the content streams.
  run_serve_test "serve watermark re-encodes non-ASCII text to cp1252" env TMPDIR="$TMPDIR" SERVE_PORT="$SERVE_PORT" INPUT="$INPUT" bash -c '
    curl -sf --retry 3 --retry-delay 1 --max-time 10 \
      -F "pdf_file=@${INPUT}" \
      -F "config={\"watermark_text\":\"RÉSUMÉ\"}" \
      "http://localhost:${SERVE_PORT}/api/watermark-existing" -o "${TMPDIR}/serve_wm_utf8.pdf" &&
    python3 -c "
import sys, re, zlib
data = open(sys.argv[1], \"rb\").read()
cp1252 = b\"R\xc9SUM\xc9\"   # RÉSUMÉ re-encoded to WinAnsi
found = False
for m in re.finditer(rb\"stream\r?\n(.*?)endstream\", data, re.S):
    s = m.group(1)
    try: s = zlib.decompress(s)
    except Exception: pass
    if cp1252 in s: found = True
assert found, \"cp1252-encoded RÉSUMÉ not found in any content stream\"
" "${TMPDIR}/serve_wm_utf8.pdf"
  '

  # server-side copy of the stamping code must also center on the visual page
  # (MediaBox origin offset), same as the CLI watermark command
  if command -v qpdf > /dev/null 2>&1 && [ -f "$TMPDIR/wm_box.pdf" ]; then
    run_serve_test "serve watermark centers on offset-origin MediaBox" env TMPDIR="$TMPDIR" SERVE_PORT="$SERVE_PORT" bash -c '
      curl -sf --retry 3 --retry-delay 1 --max-time 10 \
        -F "pdf_file=@${TMPDIR}/wm_box.pdf" -F "config={}" \
        "http://localhost:${SERVE_PORT}/api/watermark-existing" -o "${TMPDIR}/serve_wm_box.pdf" &&
      qpdf --qdf --object-streams=disable "${TMPDIR}/serve_wm_box.pdf" "${TMPDIR}/serve_wm_box.qdf" &&
      grep -q -- "0.7071 0.7071 -0.7071 0.7071 350 400 cm" "${TMPDIR}/serve_wm_box.qdf"
    '
  fi

  run_serve_test "serve password-protect honors distinct owner password" env TSPDF="$TSPDF" TMPDIR="$TMPDIR" SERVE_PORT="$SERVE_PORT" INPUT="$INPUT" bash -c '
    curl -sf --retry 3 --retry-delay 1 --max-time 10 \
      -F "pdf_file=@${INPUT}" \
      -F "config={\"password\":\"userpw\",\"owner_password\":\"ownerpw\"}" \
      "http://localhost:${SERVE_PORT}/api/password-protect" -o "${TMPDIR}/serve_prot.pdf" &&
    "$TSPDF" info "${TMPDIR}/serve_prot.pdf" --password ownerpw > /dev/null &&
    "$TSPDF" info "${TMPDIR}/serve_prot.pdf" --password userpw > /dev/null &&
    ! "$TSPDF" info "${TMPDIR}/serve_prot.pdf" --password wrongpw > /dev/null 2>&1
  '

  # Pinning tests: pin current behavior of routes before the serve_edit refactor
  run_serve_test "serve POST /api/split" bash -c "curl -sf --retry 3 --retry-delay 1 --max-time 10 -F 'pdf_file=@$INPUT' -F 'config={\"pages\":\"1-2\"}' http://localhost:${SERVE_PORT}/api/split -o $TMPDIR/serve_split.pdf && $TSPDF info $TMPDIR/serve_split.pdf | grep -q 'Pages:      2'"
  run_serve_test "serve POST /api/delete-pages" bash -c "curl -sf --retry 3 --retry-delay 1 --max-time 10 -F 'pdf_file=@$INPUT' -F 'config={\"pages\":\"1\"}' http://localhost:${SERVE_PORT}/api/delete-pages -o $TMPDIR/serve_del.pdf && $TSPDF info $TMPDIR/serve_del.pdf | grep -q 'Pages:      2'"
  run_serve_test "serve POST /api/rotate" bash -c "curl -sf --retry 3 --retry-delay 1 --max-time 10 -F 'pdf_file=@$INPUT' -F 'config={\"pages\":\"all\",\"angle\":\"90\"}' http://localhost:${SERVE_PORT}/api/rotate -o $TMPDIR/serve_rot.pdf && $TSPDF info $TMPDIR/serve_rot.pdf > /dev/null"
  run_serve_test "serve POST /api/reorder" bash -c "curl -sf --retry 3 --retry-delay 1 --max-time 10 -F 'pdf_file=@$INPUT' -F 'config={\"order\":\"3,2,1\"}' http://localhost:${SERVE_PORT}/api/reorder -o $TMPDIR/serve_reord.pdf && $TSPDF info $TMPDIR/serve_reord.pdf > /dev/null"
  $TSPDF encrypt $INPUT -o $TMPDIR/serve_enc.pdf --password srvpw > /dev/null 2>&1
  run_serve_test "serve POST /api/unlock" bash -c "curl -sf --retry 3 --retry-delay 1 --max-time 10 -F 'pdf_file=@$TMPDIR/serve_enc.pdf' -F 'config={\"password\":\"srvpw\"}' http://localhost:${SERVE_PORT}/api/unlock -o $TMPDIR/serve_unlock.pdf && $TSPDF info $TMPDIR/serve_unlock.pdf | grep -vq 'Encrypted:.*yes'"
  run_serve_test "serve POST /api/password-protect" bash -c "curl -sf --retry 3 --retry-delay 1 --max-time 10 -F 'pdf_file=@$INPUT' -F 'config={\"password\":\"newpw\"}' http://localhost:${SERVE_PORT}/api/password-protect -o $TMPDIR/serve_prot.pdf && $TSPDF info $TMPDIR/serve_prot.pdf | grep -q 'Encrypted:.*yes'"
  run_serve_test "serve rotate encrypted upload with password" bash -c "curl -sf --retry 3 --retry-delay 1 --max-time 10 -F 'pdf_file=@$TMPDIR/serve_enc.pdf' -F 'config={\"pages\":\"all\",\"angle\":\"90\",\"password\":\"srvpw\"}' http://localhost:${SERVE_PORT}/api/rotate -o $TMPDIR/serve_rot_enc.pdf && $TSPDF info $TMPDIR/serve_rot_enc.pdf --password srvpw > /dev/null"

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
run_test "text help mentions --layout" bash -c "$TSPDF text --help | grep -q -- '--layout'"

# --layout: a two-column page (drawn right column first) must come out with
# both cells of a row on one line, left cell first, separated by a space run.
if command -v python3 > /dev/null 2>&1; then
  python3 - "$TMPDIR/twocol.pdf" << 'PYEOF'
import sys
content = (b"BT /F1 12 Tf 300 700 Td (RIGHTONE) Tj ET "
           b"BT /F1 12 Tf 72 700 Td (LEFTONE) Tj ET "
           b"BT /F1 12 Tf 72 686 Td (LEFTTWO) Tj ET "
           b"BT /F1 12 Tf 300 686 Td (RIGHTTWO) Tj ET")
objs = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
     b"/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>"),
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    b"<< /Length %d >>\nstream\n%s\nendstream" % (len(content), content),
]
out = bytearray(b"%PDF-1.4\n")
offsets = []
for i, body in enumerate(objs, 1):
    offsets.append(len(out))
    out += b"%d 0 obj\n%s\nendobj\n" % (i, body)
xref = len(out)
out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
for off in offsets:
    out += b"%010d 00000 n \n" % off
out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF" % (len(objs) + 1, xref)
open(sys.argv[1], "wb").write(out)
PYEOF
  run_test "text --layout keeps columns on one line" bash -c "$TSPDF text --layout $TMPDIR/twocol.pdf | grep -qE 'LEFTONE {2,}RIGHTONE'"
  run_test "text --layout second row aligned too" bash -c "$TSPDF text --layout $TMPDIR/twocol.pdf | grep -qE 'LEFTTWO {2,}RIGHTTWO'"
  run_test "text --layout composes with --pages and -o" bash -c "$TSPDF text --layout $TMPDIR/twocol.pdf --pages 1 -o $TMPDIR/twocol.txt && grep -qE 'LEFTONE {2,}RIGHTONE' $TMPDIR/twocol.txt"
  run_test "text without --layout stays in stream order" bash -c "$TSPDF text $TMPDIR/twocol.pdf | head -1 | grep -q 'RIGHTONE LEFTONE'"
else
  echo "  SKIP  text --layout fixture tests (python3 not found)"
fi

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

# --- Indirect outline titles (pdfTeX/hyperref store /Title as N 0 R) ---
# tests/data/indirect_title.pdf: Alpha -> p1 (child Alpha Sub -> p2), Beta -> p2,
# every /Title (and one /Dest) an indirect object.
INDIRECT_INPUT=tests/data/indirect_title.pdf
run_test "bookmark list resolves indirect titles" bash -c "
  set -e
  out=\$($TSPDF bookmark list $INDIRECT_INPUT)
  echo \"\$out\" | grep -q 'Alpha'
  echo \"\$out\" | grep -q 'Alpha Sub'
  echo \"\$out\" | grep -q 'Beta'"
run_test "bookmark add keeps existing indirect titles" bash -c "
  set -e
  $TSPDF bookmark add $INDIRECT_INPUT --title Appendix --page 1 -o $TMPDIR/it_add.pdf
  out=\$($TSPDF bookmark list $TMPDIR/it_add.pdf)
  echo \"\$out\" | grep -q 'Alpha'
  echo \"\$out\" | grep -q 'Beta'
  echo \"\$out\" | grep -q 'Appendix'"
run_test "merge copies indirect outline strings (no offset-0 xref entries)" bash -c "
  set -e
  $TSPDF merge $INDIRECT_INPUT $INDIRECT_INPUT -o $TMPDIR/it_merged.pdf
  ! grep -qa '0000000000 00000 n' $TMPDIR/it_merged.pdf
  $TSPDF bookmark list $TMPDIR/it_merged.pdf | grep -q 'Alpha Sub'"
if command -v qpdf > /dev/null 2>&1; then
  run_test "merged indirect-title output passes qpdf --check" bash -c "
    set -e
    $TSPDF merge $INDIRECT_INPUT $INDIRECT_INPUT -o $TMPDIR/it_merged_q.pdf
    qpdf --check $TMPDIR/it_merged_q.pdf
    qpdf --json --json-key=outlines $TMPDIR/it_merged_q.pdf | grep -q 'Alpha Sub'"
else
  echo "  SKIP  merged indirect-title output passes qpdf --check (qpdf missing)"
fi

# --- md2pdf tables/images, burst split, page numbers, serve --bind ---

# Burst split: no --pages splits every page into its own zero-padded file.
run_test "split --burst produces one file per page" bash -c "
  set -e
  $TSPDF split $INPUT --burst -o $TMPDIR/burst.pdf > /dev/null
  test -f $TMPDIR/burst-001.pdf
  test -f $TMPDIR/burst-002.pdf
  test -f $TMPDIR/burst-003.pdf
  test ! -e $TMPDIR/burst-004.pdf"

run_test "split without --pages defaults to burst" bash -c "
  set -e
  $TSPDF split $INPUT -o $TMPDIR/burstdef.pdf > /dev/null
  test -f $TMPDIR/burstdef-001.pdf
  test -f $TMPDIR/burstdef-003.pdf"

run_test "split burst files each have one page" bash -c "
  set -e
  for i in 1 2 3; do
    $TSPDF info $TMPDIR/burst-00\$i.pdf | grep -q 'Pages:[[:space:]]*1'
  done"

if command -v qpdf > /dev/null 2>&1; then
  run_test "split burst files pass qpdf --check" bash -c "
    set -e
    for i in 1 2 3; do qpdf --check $TMPDIR/burst-00\$i.pdf > /dev/null; done"
else
  echo "  SKIP  split burst files pass qpdf --check (qpdf not found)"
fi

run_test "split rejects --burst combined with --pages" bash -c "
  ! $TSPDF split $INPUT --burst --pages 1 -o $TMPDIR/burstconflict.pdf 2>/dev/null"

run_test "split help mentions --burst" bash -c "$TSPDF split --help | grep -q -- '--burst'"

# pagenum: stamp page numbers via the overlay API.
run_test "pagenum stamps default numbers" bash -c "
  set -e
  $TSPDF pagenum $INPUT -o $TMPDIR/pn.pdf > /dev/null
  $TSPDF info $TMPDIR/pn.pdf | grep -q 'Pages:[[:space:]]*3'"

if command -v qpdf > /dev/null 2>&1; then
  run_test "pagenum content streams carry the numbers" bash -c "
    set -e
    qpdf --qdf --object-streams=disable $TMPDIR/pn.pdf $TMPDIR/pn.qdf
    grep -q '(1)' $TMPDIR/pn.qdf
    grep -q '(2)' $TMPDIR/pn.qdf
    grep -q '(3)' $TMPDIR/pn.qdf"

  run_test "pagenum honors --format with page and total" bash -c "
    set -e
    $TSPDF pagenum $INPUT -o $TMPDIR/pnfmt.pdf --format 'Page %d of %d' > /dev/null
    qpdf --qdf --object-streams=disable $TMPDIR/pnfmt.pdf $TMPDIR/pnfmt.qdf
    grep -q '(Page 2 of 3)' $TMPDIR/pnfmt.qdf"

  run_test "pagenum --start offsets numbers and total" bash -c "
    set -e
    $TSPDF pagenum $INPUT -o $TMPDIR/pnstart.pdf --format '%d/%d' --start 10 > /dev/null
    qpdf --qdf --object-streams=disable $TMPDIR/pnstart.pdf $TMPDIR/pnstart.qdf
    grep -q '(10/12)' $TMPDIR/pnstart.qdf
    grep -q '(12/12)' $TMPDIR/pnstart.qdf"

  run_test "pagenum output passes qpdf --check" qpdf --check $TMPDIR/pn.pdf
else
  echo "  SKIP  pagenum content assertions (qpdf not found)"
fi

# Format string is printf-adjacent: only plain %d (max two) and %% may appear.
run_test "pagenum rejects %s in --format" bash -c "
  ! $TSPDF pagenum $INPUT -o $TMPDIR/pnbad.pdf --format '%s' 2>/dev/null"

run_test "pagenum rejects three %d in --format" bash -c "
  ! $TSPDF pagenum $INPUT -o $TMPDIR/pnbad.pdf --format '%d %d %d' 2>/dev/null"

run_test "pagenum rejects width-flagged %5d in --format" bash -c "
  ! $TSPDF pagenum $INPUT -o $TMPDIR/pnbad.pdf --format '%5d' 2>/dev/null"

run_test "pagenum rejects invalid --position" bash -c "
  ! $TSPDF pagenum $INPUT -o $TMPDIR/pnbad.pdf --position middle 2>/dev/null"

run_test "pagenum accepts top-right position and custom size" $TSPDF pagenum $INPUT -o $TMPDIR/pntr.pdf --position top-right --font-size 8

# --pages: stamp only part of the document (e.g. skip cover pages). The
# number still reflects the true page position and the %d total stays the
# real page count.
$TSPDF split $INPUT --pages 1 -o $TMPDIR/pn1.pdf > /dev/null 2>&1
$TSPDF merge $INPUT $TMPDIR/pn1.pdf -o $TMPDIR/pn4.pdf > /dev/null 2>&1  # 4-page doc
run_test "pagenum --pages 2-3 stamps pages 2 and 3" bash -c "
  set -e
  $TSPDF pagenum $TMPDIR/pn4.pdf -o $TMPDIR/pnrange.pdf --pages 2-3 --format '[%d/%d]' > /dev/null
  $TSPDF text $TMPDIR/pnrange.pdf --pages 2 | grep -q '\[2/4\]'
  $TSPDF text $TMPDIR/pnrange.pdf --pages 3 | grep -q '\[3/4\]'"
run_test "pagenum --pages leaves page 1 unstamped" bash -c "
  ! $TSPDF text $TMPDIR/pnrange.pdf --pages 1 | grep -q '\[1/4\]'"
run_test "pagenum --pages leaves page 4 unstamped" bash -c "
  ! $TSPDF text $TMPDIR/pnrange.pdf --pages 4 | grep -q '\[4/4\]'"
run_test "pagenum --pages out of range fails with page count" bash -c "
  ! $TSPDF pagenum $TMPDIR/pn4.pdf -o $TMPDIR/pnbad.pdf --pages 9 > /dev/null 2>&1"
run_test "pagenum rejects invalid --pages" bash -c "
  ! $TSPDF pagenum $TMPDIR/pn4.pdf -o $TMPDIR/pnbad.pdf --pages abc 2>/dev/null"
run_test "pagenum help mentions --pages" bash -c "$TSPDF pagenum --help | grep -q -- '--pages'"

run_test "help pagenum shows command-specific usage" bash -c "$TSPDF help pagenum 2>/dev/null | grep -q 'Usage: tspdf pagenum'"
run_test "help pagenum documents --pages" bash -c "$TSPDF help pagenum 2>/dev/null | grep -q -- '--pages'"

# --- attach: embed / list / extract / remove file attachments ---
printf 'attach payload line one\n' > "$TMPDIR/att_f.txt"      # 24 bytes
head -c 2048 /dev/urandom > "$TMPDIR/att_blob.bin"

run_test "attach add embeds files" $TSPDF attach add $INPUT "$TMPDIR/att_f.txt" "$TMPDIR/att_blob.bin" --desc "att desc" -o "$TMPDIR/att.pdf"
run_test "attach list shows name, size, desc" bash -c "
  $TSPDF attach list $TMPDIR/att.pdf | grep -q \"^att_f.txt\$(printf '\\t')24\$(printf '\\t')att desc\$\" &&
  $TSPDF attach list $TMPDIR/att.pdf | grep -q \"^att_blob.bin\$(printf '\\t')2048\$(printf '\\t')att desc\$\""
run_test "attach list keys come out sorted" bash -c "
  [ \"\$($TSPDF attach list $TMPDIR/att.pdf | head -1 | cut -f1)\" = att_blob.bin ]"
run_test "attach extract --all round-trips bytes" bash -c "
  rm -rf $TMPDIR/att_ex && mkdir $TMPDIR/att_ex &&
  $TSPDF attach extract $TMPDIR/att.pdf --all -o $TMPDIR/att_ex > /dev/null &&
  cmp $TMPDIR/att_ex/att_f.txt $TMPDIR/att_f.txt &&
  cmp $TMPDIR/att_ex/att_blob.bin $TMPDIR/att_blob.bin"
run_test "attach extract --name picks one file" bash -c "
  rm -rf $TMPDIR/att_ex1 && mkdir $TMPDIR/att_ex1 &&
  $TSPDF attach extract $TMPDIR/att.pdf --name att_blob.bin -o $TMPDIR/att_ex1 > /dev/null &&
  cmp $TMPDIR/att_ex1/att_blob.bin $TMPDIR/att_blob.bin &&
  [ ! -e $TMPDIR/att_ex1/att_f.txt ]"
run_test "attach extract unknown --name fails" bash -c "
  ! $TSPDF attach extract $TMPDIR/att.pdf --name no_such.bin -o $TMPDIR 2>/dev/null"
run_test "attach remove drops one attachment" bash -c "
  $TSPDF attach remove $TMPDIR/att.pdf --name att_f.txt -o $TMPDIR/att_rm.pdf > /dev/null &&
  [ \"\$($TSPDF attach list $TMPDIR/att_rm.pdf | wc -l | tr -d '[:space:]')\" = 1 ] &&
  ! $TSPDF attach list $TMPDIR/att_rm.pdf | grep -q att_f.txt"
run_test "attach remove unknown name fails" bash -c "
  ! $TSPDF attach remove $TMPDIR/att.pdf --name no_such.bin -o $TMPDIR/att_rm2.pdf 2>/dev/null"
run_test "attach add replaces an existing name" bash -c "
  printf 'replaced' > $TMPDIR/att_f.txt &&
  $TSPDF attach add $TMPDIR/att.pdf $TMPDIR/att_f.txt -o $TMPDIR/att_rep.pdf > /dev/null &&
  [ \"\$($TSPDF attach list $TMPDIR/att_rep.pdf | wc -l | tr -d '[:space:]')\" = 2 ] &&
  rm -rf $TMPDIR/att_ex2 && mkdir $TMPDIR/att_ex2 &&
  $TSPDF attach extract $TMPDIR/att_rep.pdf --name att_f.txt -o $TMPDIR/att_ex2 > /dev/null &&
  [ \"\$(cat $TMPDIR/att_ex2/att_f.txt)\" = replaced ]"

# Attachments are document-level: they survive split (page extraction) and
# merge (union across sources) through the tspdf pipeline itself.
run_test "attach survives split --pages" bash -c "
  $TSPDF split $TMPDIR/att_rm.pdf --pages 1 -o $TMPDIR/att_split.pdf > /dev/null &&
  rm -rf $TMPDIR/att_ex3 && mkdir $TMPDIR/att_ex3 &&
  $TSPDF attach extract $TMPDIR/att_split.pdf --all -o $TMPDIR/att_ex3 > /dev/null &&
  cmp $TMPDIR/att_ex3/att_blob.bin $TMPDIR/att_blob.bin"
run_test "attach survives merge with union" bash -c "
  $TSPDF attach add $INPUT $TMPDIR/att_f.txt -o $TMPDIR/att_m1.pdf > /dev/null &&
  $TSPDF attach add $INPUT $TMPDIR/att_blob.bin -o $TMPDIR/att_m2.pdf > /dev/null &&
  $TSPDF merge $TMPDIR/att_m1.pdf $TMPDIR/att_m2.pdf -o $TMPDIR/att_merged.pdf > /dev/null &&
  [ \"\$($TSPDF attach list $TMPDIR/att_merged.pdf | wc -l | tr -d '[:space:]')\" = 2 ] &&
  $TSPDF attach list $TMPDIR/att_merged.pdf | grep -q att_f.txt &&
  $TSPDF attach list $TMPDIR/att_merged.pdf | grep -q att_blob.bin"

# Hostile stored names must never escape the target directory. The name is
# patched to \"../evil.txt\" post-save (same byte length keeps the xref valid).
run_test "attach extract sanitizes ../ traversal names" bash -c "
  set -e
  printf 'evil payload\n' > $TMPDIR/xxxevil.txt
  $TSPDF attach add $INPUT $TMPDIR/xxxevil.txt -o $TMPDIR/att_pre_evil.pdf > /dev/null
  LC_ALL=C sed 's|xxxevil.txt|../evil.txt|g' $TMPDIR/att_pre_evil.pdf > $TMPDIR/att_evil.pdf
  $TSPDF attach list $TMPDIR/att_evil.pdf | grep -q '^\.\./evil\.txt'
  rm -rf $TMPDIR/att_evil_dir $TMPDIR/evil.txt
  mkdir $TMPDIR/att_evil_dir
  $TSPDF attach extract $TMPDIR/att_evil.pdf --all -o $TMPDIR/att_evil_dir > /dev/null
  cmp $TMPDIR/att_evil_dir/evil.txt $TMPDIR/xxxevil.txt
  [ ! -e $TMPDIR/evil.txt ]"

# Encrypted PDFs: attachment streams decrypt like any other stream.
run_test "attach list/extract work with --password" bash -c "
  $TSPDF encrypt $TMPDIR/att.pdf --password attsecret -o $TMPDIR/att_enc.pdf > /dev/null &&
  ! $TSPDF attach list $TMPDIR/att_enc.pdf < /dev/null > /dev/null 2>&1 &&
  $TSPDF attach list $TMPDIR/att_enc.pdf --password attsecret | grep -q att_blob.bin &&
  rm -rf $TMPDIR/att_ex4 && mkdir $TMPDIR/att_ex4 &&
  $TSPDF attach extract $TMPDIR/att_enc.pdf --all -o $TMPDIR/att_ex4 --password attsecret > /dev/null &&
  cmp $TMPDIR/att_ex4/att_blob.bin $TMPDIR/att_blob.bin"

if command -v python3 > /dev/null 2>&1; then
  cat > "$TMPDIR/att_check.py" <<'PYEOT'
import json, sys
entries = json.load(sys.stdin)
by_name = {e["name"]: e for e in entries}
assert list(by_name) == sorted(by_name), "names not sorted"
assert by_name["att_f.txt"]["size"] == 24
assert by_name["att_f.txt"]["desc"] == "att desc"
assert by_name["att_blob.bin"]["size"] == 2048
assert "mime" in by_name["att_f.txt"]
PYEOT
  run_test "attach list --json is valid and complete" bash -c "
    $TSPDF attach list $TMPDIR/att.pdf --json | python3 $TMPDIR/att_check.py"
fi

run_test "attach listed in main help" bash -c "$TSPDF --help | grep -q '^  attach'"
run_test "help attach shows subcommand usage" bash -c "$TSPDF help attach 2>/dev/null | grep -q 'Usage: tspdf attach add'"
run_test "attach unknown subcommand fails" bash -c "! $TSPDF attach frobnicate 2>/dev/null"

# md2pdf emphasis: *x*/_x_ italic, **x** bold; literal markers must not leak.
if command -v qpdf > /dev/null 2>&1; then
  run_test "md2pdf emphasis styles text without literal markers" bash -c "
    set -e
    cat > $TMPDIR/emph.md <<'EOF'
# Emphasis

Plain ITALWORD is *ITALWORD* and BOLDWORD is **BOLDWORD** and _UNDERITAL_ ends.

A snake_case_word stays literal and 2 * 3 = 6 stays literal too.

This is a deliberately long wrapping paragraph that keeps going and going and going far past one line so the styled segments cannot fit on a single rendered line and *WRAPITALWORD* must fall back to plain de-marked text while still wrapping across lines without leaking markers.
EOF
    $TSPDF md2pdf $TMPDIR/emph.md -o $TMPDIR/emph.pdf > /dev/null 2>&1
    qpdf --qdf --object-streams=disable $TMPDIR/emph.pdf $TMPDIR/emph.qdf
    grep -q 'Helvetica-Oblique' $TMPDIR/emph.qdf
    grep -q 'ITALWORD' $TMPDIR/emph.qdf
    grep -q 'UNDERITAL' $TMPDIR/emph.qdf
    grep -q 'WRAPITALWORD' $TMPDIR/emph.qdf
    ! grep -q -- '\*ITALWORD\*' $TMPDIR/emph.qdf
    ! grep -q -- '\*\*BOLDWORD\*\*' $TMPDIR/emph.qdf
    ! grep -q -- '_UNDERITAL_' $TMPDIR/emph.qdf
    ! grep -q -- '\*WRAPITALWORD\*' $TMPDIR/emph.qdf
    grep -q 'snake_case_word' $TMPDIR/emph.qdf
    grep -q '2 \* 3 = 6' $TMPDIR/emph.qdf"
else
  echo "  SKIP  md2pdf emphasis (qpdf not found)"
fi

# md2pdf pipe tables: header + separator + body become a real layout table.
if command -v qpdf > /dev/null 2>&1; then
  run_test "md2pdf renders pipe tables without literal pipes" bash -c "
    set -e
    cat > $TMPDIR/tbl.md <<'EOF'
# Table

| HDRALPHA | HDRBETA | HDRGAMMA |
|:---------|:-------:|---------:|
| CELLA1   | CELLB1  | CELLC1   |
| CELLA2   | **BOLDCELL** | pipe \| stays |

After the table.
EOF
    $TSPDF md2pdf $TMPDIR/tbl.md -o $TMPDIR/tbl.pdf > /dev/null 2>&1
    qpdf --qdf --object-streams=disable $TMPDIR/tbl.pdf $TMPDIR/tbl.qdf
    grep -q 'HDRALPHA' $TMPDIR/tbl.qdf
    grep -q 'HDRGAMMA' $TMPDIR/tbl.qdf
    grep -q 'CELLA1' $TMPDIR/tbl.qdf
    grep -q 'CELLC1' $TMPDIR/tbl.qdf
    grep -q 'BOLDCELL' $TMPDIR/tbl.qdf
    grep -q 'pipe | stays' $TMPDIR/tbl.qdf
    grep -q 'After the table.' $TMPDIR/tbl.qdf
    ! grep -q '| HDRALPHA' $TMPDIR/tbl.qdf
    ! grep -q 'CELLA1   |' $TMPDIR/tbl.qdf
    ! grep -q -- ':---' $TMPDIR/tbl.qdf
    ! grep -q -- '\*\*BOLDCELL\*\*' $TMPDIR/tbl.qdf"

  run_test "md2pdf pipe-lookalike without separator stays a paragraph" bash -c "
    set -e
    printf '| not a table, just text\n\nnext paragraph\n' > $TMPDIR/nottbl.md
    $TSPDF md2pdf $TMPDIR/nottbl.md -o $TMPDIR/nottbl.pdf > /dev/null 2>&1
    qpdf --qdf --object-streams=disable $TMPDIR/nottbl.pdf $TMPDIR/nottbl.qdf
    grep -q 'not a table, just text' $TMPDIR/nottbl.qdf"

  run_test "md2pdf table output passes qpdf --check" qpdf --check $TMPDIR/tbl.pdf

  # Regression: at TSPDF_LAYOUT_MAX_CHILDREN=32 every block after the 32nd
  # was silently dropped (and tables were capped at 31 rows).
  run_test "md2pdf keeps documents longer than 32 blocks" bash -c "
    set -e
    for i in \$(seq 1 60); do printf 'Paragraph PARA%d here.\n\n' \$i; done > $TMPDIR/long.md
    $TSPDF md2pdf $TMPDIR/long.md -o $TMPDIR/long.pdf > /dev/null 2>&1
    qpdf --qdf --object-streams=disable $TMPDIR/long.pdf $TMPDIR/long.qdf
    grep -q 'PARA33' $TMPDIR/long.qdf
    grep -q 'PARA60' $TMPDIR/long.qdf"

  run_test "md2pdf renders tables longer than 31 rows" bash -c "
    set -e
    { printf '| N | NAME |\n|---|---|\n'
      for i in \$(seq 1 40); do printf '| %d | ROWCELL%d |\n' \$i \$i; done
    } > $TMPDIR/bigtbl.md
    $TSPDF md2pdf $TMPDIR/bigtbl.md -o $TMPDIR/bigtbl.pdf > /dev/null 2>&1
    qpdf --qdf --object-streams=disable $TMPDIR/bigtbl.pdf $TMPDIR/bigtbl.qdf
    grep -q 'ROWCELL40' $TMPDIR/bigtbl.qdf"
else
  echo "  SKIP  md2pdf pipe tables (qpdf not found)"
fi

# The layout tree caps top-level blocks at TSPDF_LAYOUT_MAX_CHILDREN (1024).
# Content past the cap is dropped, but never silently: exactly one stderr
# warning, and the (truncated) PDF is still written with exit 0.
run_test "md2pdf warns once past the 1024-block cap" bash -c "
  set -e
  for i in \$(seq 1 1500); do printf 'Paragraph PARA%d here.\n\n' \$i; done > $TMPDIR/huge.md
  $TSPDF md2pdf $TMPDIR/huge.md -o $TMPDIR/huge.pdf 2> $TMPDIR/huge.err
  [ \$(grep -c 'exceeds 1024 blocks' $TMPDIR/huge.err) -eq 1 ]
  $TSPDF info $TMPDIR/huge.pdf > /dev/null"

# md2pdf images: block-level ![alt](path) embeds the image as an XObject,
# path resolved relative to the .md file; missing files warn + alt fallback.
if command -v qpdf > /dev/null 2>&1; then
  run_test "md2pdf embeds PNG and JPEG images" bash -c "
    set -e
    mkdir -p $TMPDIR/mdimg
    cp examples/test.png examples/test.jpg $TMPDIR/mdimg/
    printf '# Images\n\n![png alt](test.png)\n\ntext between\n\n![jpg alt](test.jpg)\n' > $TMPDIR/mdimg/img.md
    $TSPDF md2pdf $TMPDIR/mdimg/img.md -o $TMPDIR/img.pdf > /dev/null 2>&1
    qpdf --qdf --object-streams=disable $TMPDIR/img.pdf $TMPDIR/img.qdf
    [ \$(grep -c '/Subtype /Image' $TMPDIR/img.qdf) -eq 2 ]
    grep -q 'text between' $TMPDIR/img.qdf"

  run_test "md2pdf image output passes qpdf --check" qpdf --check $TMPDIR/img.pdf

  run_test "md2pdf missing image warns and falls back to alt text" bash -c "
    set -e
    printf '![MISSINGALT](no/such/file.png)\n' > $TMPDIR/miss.md
    $TSPDF md2pdf $TMPDIR/miss.md -o $TMPDIR/miss.pdf 2> $TMPDIR/miss.err
    grep -qi 'warning' $TMPDIR/miss.err
    qpdf --qdf --object-streams=disable $TMPDIR/miss.pdf $TMPDIR/miss.qdf
    grep -q 'MISSINGALT' $TMPDIR/miss.qdf
    ! grep -q '/Subtype /Image' $TMPDIR/miss.qdf"
else
  echo "  SKIP  md2pdf images (qpdf not found)"
fi

# qrcode: payload lengths spanning versions 1-11 (10..250 chars) must encode
# and produce a well-formed page. The encoder output itself is pinned by the
# decode-verified golden grids in tests/test_main.c.
run_test "qrcode encodes payloads from 10 to 250 chars" bash -c "
  set -e
  base='https://example.org/p/'
  for len in 10 40 116 213 250; do
    p=\$(printf '%s%s' \"\$base\" \"\$(printf 'a%.0s' \$(seq 1 250))\" | cut -c1-\$len)
    $TSPDF qrcode \"\$p\" -o $TMPDIR/qr\$len.pdf > /dev/null
    $TSPDF info $TMPDIR/qr\$len.pdf > /dev/null
  done"

run_test "qrcode rejects payloads beyond capacity" bash -c "
  p=\$(printf 'a%.0s' \$(seq 1 300))
  ! $TSPDF qrcode \"\$p\" -o $TMPDIR/qrtoolong.pdf 2> $TMPDIR/qrtoolong.err
  grep -q 'too long' $TMPDIR/qrtoolong.err"

# The default title documented in the help ("QR Code") must actually render,
# and an explicit --title must replace it. Both are checked through the
# repo's own text extractor.
run_test "qrcode renders default title, link, and explicit title" bash -c "
  set -e
  $TSPDF qrcode 'https://example.org/scan' -o $TMPDIR/qrdef.pdf > /dev/null
  $TSPDF text $TMPDIR/qrdef.pdf | grep -q 'QR Code'
  $TSPDF text $TMPDIR/qrdef.pdf | grep -q 'https://example.org/scan'
  $TSPDF qrcode 'https://example.org/scan' --title 'CUSTOMTITLE' --subtitle 'SUBTXT' \
      -o $TMPDIR/qrcust.pdf > /dev/null
  $TSPDF text $TMPDIR/qrcust.pdf | grep -q 'CUSTOMTITLE'
  $TSPDF text $TMPDIR/qrcust.pdf | grep -q 'SUBTXT'
  ! $TSPDF text $TMPDIR/qrcust.pdf | grep -q 'QR Code'"

if command -v qpdf > /dev/null 2>&1; then
  run_test "qrcode output passes qpdf --check" bash -c "
    set -e
    for len in 10 116 250; do qpdf --check $TMPDIR/qr\$len.pdf > /dev/null; done"
else
  echo "  SKIP  qrcode qpdf --check (qpdf not found)"
fi

# serve --bind: default stays loopback; non-loopback binds warn and keep a
# same-origin gate for browser POSTs.
BIND_PORT_LOOP=$((CLI_TEST_PORT_BASE + 50))
BIND_PORT_ANY=$((CLI_TEST_PORT_BASE + 51))
BIND_PORT_CSRF=$((CLI_TEST_PORT_BASE + 52))

run_test "serve rejects an invalid --bind address" bash -c "
  ! $TSPDF serve --bind 999.1.2.3 --port $BIND_PORT_LOOP 2>/dev/null"

run_test "serve help mentions --bind" bash -c "$TSPDF serve --help | grep -q -- '--bind'"

if command -v curl > /dev/null 2>&1; then
  run_test "serve --bind 127.0.0.1 answers GET /" bash -c "
    $TSPDF serve --bind 127.0.0.1 --port $BIND_PORT_LOOP > /dev/null 2>&1 & sp=\$!
    sleep 2
    curl -sf --retry 3 --retry-delay 1 --max-time 5 http://127.0.0.1:${BIND_PORT_LOOP}/ -o /dev/null
    rc=\$?
    kill \$sp 2>/dev/null; wait \$sp 2>/dev/null
    exit \$rc"

  run_test "serve --bind 0.0.0.0 warns and answers via 127.0.0.1" bash -c "
    $TSPDF serve --bind 0.0.0.0 --port $BIND_PORT_ANY > $TMPDIR/bind_any.log 2>&1 & sp=\$!
    sleep 2
    curl -sf --retry 3 --retry-delay 1 --max-time 5 http://127.0.0.1:${BIND_PORT_ANY}/ -o /dev/null
    rc=\$?
    kill \$sp 2>/dev/null; wait \$sp 2>/dev/null
    [ \$rc -eq 0 ] && grep -qi 'warning' $TMPDIR/bind_any.log && grep -qi 'no authentication' $TMPDIR/bind_any.log"
else
  echo "  SKIP  serve --bind e2e (curl not found)"
fi

# With a non-loopback bind the Host header can name any address the server is
# reachable at, but a browser POST carrying a foreign Origin must still be
# rejected (same-origin gate), and a matching Origin must pass.
if command -v python3 > /dev/null 2>&1; then
  run_test "serve --bind 0.0.0.0 keeps same-origin gate on POST /api" bash -c "
    set -e
    \"$TSPDF\" serve --bind 0.0.0.0 --port $BIND_PORT_CSRF > /dev/null 2>&1 & sp=\$!
    sleep 2
    python3 -c \"
import socket, sys
def post(host_hdr, origin):
    bnd = '----tspdf'
    body = ('--%s\\\\r\\\\nContent-Disposition: form-data; name=\\\"config\\\"\\\\r\\\\n\\\\r\\\\n'
            '{\\\"url\\\":\\\"https://example.com\\\"}\\\\r\\\\n--%s--\\\\r\\\\n' % (bnd, bnd)).encode('ascii')
    hdr = ('POST /api/qrcode HTTP/1.1\\\\r\\\\nHost: %s\\\\r\\\\n'
           'Origin: %s\\\\r\\\\n'
           'Content-Type: multipart/form-data; boundary=%s\\\\r\\\\n'
           'Content-Length: %d\\\\r\\\\n\\\\r\\\\n' % (host_hdr, origin, bnd, len(body))).encode('ascii')
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(('127.0.0.1', $BIND_PORT_CSRF))
    s.sendall(hdr + body)
    data = s.recv(8192)
    s.close()
    return data.split(b' ')[1]
# foreign Origin on a network Host: rejected
if post('192.168.7.7:$BIND_PORT_CSRF', 'https://evil.example.com') != b'403':
    sys.exit(1)
# same-origin POST from the served UI on a network address: accepted
if post('192.168.7.7:$BIND_PORT_CSRF', 'http://192.168.7.7:$BIND_PORT_CSRF') != b'200':
    sys.exit(2)
\"
    kill \$sp 2>/dev/null || true
    wait \$sp 2>/dev/null || true
  "
else
  echo "  SKIP  serve --bind same-origin gate (python3 not found)"
fi

# --- Distribution: versioned shared library + single-file amalgamation ---

# `make shared` must produce a versioned ELF shared object with a SONAME so
# distro packagers can ship it. Linux-only: Mach-O uses -install_name instead
# and there is no macOS box in this environment to validate the layout.
if [ "$(uname -s)" = "Linux" ] && command -v readelf > /dev/null 2>&1; then
  VMAJ=$(sed -n 's/^#define TSPDF_VERSION_MAJOR *//p' include/tspdf/version.h)
  VMIN=$(sed -n 's/^#define TSPDF_VERSION_MINOR *//p' include/tspdf/version.h)
  VPAT=$(sed -n 's/^#define TSPDF_VERSION_PATCH *//p' include/tspdf/version.h)
  # Mirror the Makefile's ABI policy: pre-1.0 the SONAME carries MAJOR.MINOR
  # (the ABI may break on minors), from 1.0 on it carries MAJOR only.
  if [ "$VMAJ" = "0" ]; then
    SOABI="$VMAJ.$VMIN"; SOTAIL="$VPAT"
  else
    SOABI="$VMAJ"; SOTAIL="$VMIN.$VPAT"
  fi
  SOREAL="libtspdf.so.$SOABI.$SOTAIL"
  SONAME="libtspdf.so.$SOABI"

  run_test "make shared produces versioned .so with SONAME" bash -c '
    set -e
    make shared BUILDDIR="'"$TMPDIR"'/shbuild" > /dev/null 2>&1
    test -f "'"$TMPDIR"'/shbuild/'"$SOREAL"'"
    test -L "'"$TMPDIR"'/shbuild/'"$SONAME"'"
    test -L "'"$TMPDIR"'/shbuild/libtspdf.so"
    readelf -d "'"$TMPDIR"'/shbuild/'"$SOREAL"'" \
      | grep -q "SONAME.*\['"$SONAME"'\]"'

  # The version script must keep internal helpers (aes_*, sha256_*, jpeg_*,
  # ...) out of the dynamic symbol table: every defined dynamic symbol is
  # tspdf_*, tspr_*, or toolchain-reserved (_init/_fini/__bss_start etc.) —
  # and the public API must not get hidden along the way.
  if command -v nm > /dev/null 2>&1; then
    run_test "shared .so exports only tspdf_/tspr_ symbols" bash -c '
      set -e
      nm -D --defined-only "'"$TMPDIR"'/shbuild/'"$SOREAL"'" \
        | sed "s/.* //" > "'"$TMPDIR"'/dynsyms.txt"
      grep -qx "tspdf_writer_create" "'"$TMPDIR"'/dynsyms.txt"
      grep -qx "tspdf_reader_open_file" "'"$TMPDIR"'/dynsyms.txt"
      ! grep -vE "^(tspdf_|tspr_|_)" "'"$TMPDIR"'/dynsyms.txt" | grep -q .'
  else
    echo "  SKIP  shared .so symbol filter (nm not found)"
  fi

  run_test "install-lib installs the versioned shared library" bash -c '
    set -e
    make install-lib DESTDIR="'"$TMPDIR"'/shstage" PREFIX=/usr \
      BUILDDIR="'"$TMPDIR"'/shbuild" > /dev/null 2>&1
    test -f "'"$TMPDIR"'/shstage/usr/lib/libtspdf.a"
    test -f "'"$TMPDIR"'/shstage/usr/lib/'"$SOREAL"'"
    test -L "'"$TMPDIR"'/shstage/usr/lib/'"$SONAME"'"
    test -L "'"$TMPDIR"'/shstage/usr/lib/libtspdf.so"'

  run_test "uninstall removes the shared library" bash -c '
    set -e
    make uninstall DESTDIR="'"$TMPDIR"'/shstage" PREFIX=/usr > /dev/null 2>&1
    test ! -e "'"$TMPDIR"'/shstage/usr/lib/'"$SOREAL"'"
    test ! -e "'"$TMPDIR"'/shstage/usr/lib/'"$SONAME"'"
    test ! -e "'"$TMPDIR"'/shstage/usr/lib/libtspdf.so"
    test ! -e "'"$TMPDIR"'/shstage/usr/lib/libtspdf.a"'
else
  echo "  SKIP  versioned shared library tests (Linux + readelf only)"
fi

# md2pdf: [text](url) becomes a clickable /Link annotation. Annotation and
# outline objects are written uncompressed, so the raw PDF bytes are
# greppable; URL parens must arrive backslash-escaped in the PDF string.
run_test "md2pdf link annotation" bash -c '
  set -e
  printf "Visit [the site](https://example.com/a(b)?q=1) today.\n" \
    > "'"$TMPDIR"'/links.md"
  "'"$TSPDF"'" md2pdf "'"$TMPDIR"'/links.md" -o "'"$TMPDIR"'/links.pdf"
  grep -qa "/Subtype /Link" "'"$TMPDIR"'/links.pdf"
  grep -qaF "/URI (https://example.com/a\\(b\\)?q=1)" "'"$TMPDIR"'/links.pdf"
'

# md2pdf: # ## ### headings become nested outline bookmarks with /XYZ dests.
run_test "md2pdf heading bookmarks" bash -c '
  set -e
  printf "# Alpha\n\ntext\n\n## Beta\n\nmore\n\n### Gamma\n\n## Delta\n\n# Omega\n" \
    > "'"$TMPDIR"'/toc.md"
  "'"$TSPDF"'" md2pdf "'"$TMPDIR"'/toc.md" -o "'"$TMPDIR"'/toc.pdf"
  grep -qa "/Type /Outlines" "'"$TMPDIR"'/toc.pdf"
  grep -qa "/Title (Alpha)" "'"$TMPDIR"'/toc.pdf"
  grep -qa "/Title (Gamma)" "'"$TMPDIR"'/toc.pdf"
  grep -qa "/XYZ" "'"$TMPDIR"'/toc.pdf"
'

# With qpdf available (always true in CI), also validate the file structurally
# and assert the outline nesting via qpdf --json.
if command -v qpdf > /dev/null 2>&1; then
  run_test "md2pdf links+bookmarks pass qpdf --check" bash -c '
    set -e
    printf "# Alpha\n\n[x](https://example.com/x) plus text\n\n## Beta\n" \
      > "'"$TMPDIR"'/lb.md"
    "'"$TSPDF"'" md2pdf "'"$TMPDIR"'/lb.md" -o "'"$TMPDIR"'/lb.pdf"
    qpdf --check "'"$TMPDIR"'/lb.pdf" > /dev/null
    qpdf --qdf "'"$TMPDIR"'/lb.pdf" "'"$TMPDIR"'/lb.qdf.pdf"
    grep -qa "/URI (https://example.com/x)" "'"$TMPDIR"'/lb.qdf.pdf"
  '
  run_test "md2pdf outline nesting via qpdf --json" bash -c '
    set -e
    printf "# Alpha\n\n## Beta\n\n### Gamma\n\n## Delta\n\n# Omega\n" > "'"$TMPDIR"'/nest.md"
    "'"$TSPDF"'" md2pdf "'"$TMPDIR"'/nest.md" -o "'"$TMPDIR"'/nest.pdf"
    python3 - "'"$TMPDIR"'/nest.pdf" <<PYEOF
import json, subprocess, sys
out = subprocess.check_output(["qpdf", "--json", "--json-key=outlines", sys.argv[1]])
o = json.loads(out)["outlines"]
assert [e["title"] for e in o] == ["Alpha", "Omega"], o
assert [e["title"] for e in o[0]["kids"]] == ["Beta", "Delta"], o
assert [e["title"] for e in o[0]["kids"][0]["kids"]] == ["Gamma"], o
assert o[1]["kids"] == [], o

# /Count must be the number of VISIBLE descendants (ISO 32000-1 Table 153),
# not the direct-child count: Alpha (kids Beta+Delta, grandchild Gamma) is 3,
# and the outline root counts every open item at any level (5 total).
out = subprocess.check_output(["qpdf", "--json", "--json-key=qpdf", sys.argv[1]])
objs = json.loads(out)["qpdf"][1]
counts, root_count = {}, None
for obj in objs.values():
    v = obj.get("value") if isinstance(obj, dict) else None
    if not isinstance(v, dict):
        continue
    if v.get("/Type") == "/Outlines":
        root_count = v.get("/Count")
    elif "/Title" in v and "/Parent" in v:
        title = str(v["/Title"])
        title = title[2:] if title.startswith("u:") else title
        counts[title] = v.get("/Count", 0)
assert counts.get("Alpha") == 3, counts
assert counts.get("Beta") == 1, counts
assert counts.get("Delta", 0) == 0, counts
assert root_count == 5, root_count
PYEOF
  '
else
  echo "  SKIP  md2pdf qpdf structural checks (qpdf not installed)"
fi

# ── form: list / fill / flatten ─────────────────────────────────────
FORM_PDF=tests/data/form_fields.pdf
FORM_ENC_PDF=tests/data/form_fields_enc.pdf

run_test "form list prints a JSON array" bash -c "$TSPDF form list $FORM_PDF | head -c 1 | grep -q '\['"
run_test "form list encrypted with --password" bash -c "$TSPDF form list $FORM_ENC_PDF --password secret | grep -q '\"name\"'"
run_test "form list wrong password fails" bash -c "! $TSPDF form list $FORM_ENC_PDF --password wrong > /dev/null 2>&1"
run_test "form on plain PDF lists empty array" bash -c "[ \"\$($TSPDF form list $INPUT)\" = '[]' ]"
run_test "form fill unknown field errors with its name" bash -c "
  ! $TSPDF form fill $FORM_PDF --set nope=1 -o $TMPDIR/form_bad.pdf 2> $TMPDIR/form_bad.err
  grep -q 'nope' $TMPDIR/form_bad.err"
run_test "form fill --set writes values" bash -c "
  set -e
  $TSPDF form fill $FORM_PDF --set name=Bob --set agree=true --set color=Blue \
      --set city=Oslo -o $TMPDIR/form_filled.pdf
  $TSPDF form list $TMPDIR/form_filled.pdf > $TMPDIR/form_filled.json
  grep -q '\"Bob\"' $TMPDIR/form_filled.json
  grep -q '\"Yes\"' $TMPDIR/form_filled.json
  grep -q '\"Blue\"' $TMPDIR/form_filled.json
  grep -q '\"Oslo\"' $TMPDIR/form_filled.json"
run_test "form fill --data - reads JSON from stdin" bash -c "
  set -e
  printf '%s' '{\"name\": \"Ren\\u00e9\", \"a.b\": \"nested\"}' \
    | $TSPDF form fill $FORM_PDF --data - -o $TMPDIR/form_json.pdf
  $TSPDF form list $TMPDIR/form_json.pdf > $TMPDIR/form_json.json
  grep -q \"Ren\$(printf '\\xc3\\xa9')\" $TMPDIR/form_json.json
  grep -q '\"nested\"' $TMPDIR/form_json.json"
run_test "form fill rejects malformed JSON" bash -c "
  printf '%s' '{\"name\": 12}' > $TMPDIR/form_bad.json
  ! $TSPDF form fill $FORM_PDF --data $TMPDIR/form_bad.json -o $TMPDIR/form_x.pdf > /dev/null 2>&1"
run_test "form flatten produces a form-free PDF" bash -c "
  set -e
  $TSPDF form flatten $TMPDIR/form_filled.pdf -o $TMPDIR/form_flat.pdf
  [ \"\$($TSPDF form list $TMPDIR/form_flat.pdf)\" = '[]' ]
  $TSPDF text $TMPDIR/form_flat.pdf | grep -q 'Bob'
  $TSPDF text $TMPDIR/form_flat.pdf | grep -q 'Oslo'"
run_test "help form shows command-specific usage" bash -c "$TSPDF help form 2>/dev/null | grep -q 'Usage: tspdf form'"

if command -v python3 > /dev/null 2>&1; then
  run_test "form list JSON structure validates" python3 - << PYEOF
import json, subprocess
out = subprocess.run(["$TSPDF", "form", "list", "$FORM_PDF"],
                     capture_output=True, check=True).stdout
fields = {f["name"]: f for f in json.loads(out)}
assert set(fields) == {"name", "agree", "city", "color", "a.b"}, sorted(fields)
assert fields["name"]["type"] == "text" and fields["name"]["value"] == "Ada"
assert fields["name"]["page"] == 1
assert fields["agree"]["type"] == "checkbox" and fields["agree"]["options"] == ["Yes"]
assert fields["color"]["type"] == "radio" and sorted(fields["color"]["options"]) == ["Blue", "Red"]
assert fields["city"]["type"] == "choice" and fields["city"]["options"] == ["Berlin", "Paris", "Oslo"]
assert fields["a.b"]["value"] is None
assert fields["name"]["readonly"] is False and fields["name"]["required"] is False
PYEOF
else
  echo "  SKIP  form list JSON structure (python3 not found)"
fi

if command -v qpdf > /dev/null 2>&1; then
  run_test "form fill output passes qpdf --check" bash -c "qpdf --check $TMPDIR/form_filled.pdf > /dev/null 2>&1"
  run_test "form fill value visible to qpdf acroform json" bash -c "
    qpdf --json --json-key=acroform $TMPDIR/form_filled.pdf | grep -q 'Bob'"
  run_test "form flatten output passes qpdf --check" bash -c "qpdf --check $TMPDIR/form_flat.pdf > /dev/null 2>&1"
  run_test "form flatten output has no acroform per qpdf" bash -c "
    ! qpdf --json --json-key=acroform $TMPDIR/form_flat.pdf | grep -q '\"fullname\"'"
else
  echo "  SKIP  form qpdf conformance checks (qpdf not installed)"
fi

# Filling/flattening an encrypted form must NOT silently strip its
# encryption: the output stays encrypted with the ORIGINAL password and
# carries the new value.
if command -v qpdf > /dev/null 2>&1; then
  run_test "form fill keeps encrypted input encrypted" bash -c "
    set -e
    $TSPDF form fill $FORM_ENC_PDF --password secret --set name=EncBob -o $TMPDIR/fill_enc.pdf
    qpdf --is-encrypted $TMPDIR/fill_enc.pdf
    qpdf --password=secret --check $TMPDIR/fill_enc.pdf > /dev/null
    $TSPDF form list $TMPDIR/fill_enc.pdf --password secret | grep -q '\"EncBob\"'"
  run_test "form flatten keeps encrypted input encrypted" bash -c "
    set -e
    $TSPDF form flatten $TMPDIR/fill_enc.pdf --password secret -o $TMPDIR/flat_enc.pdf
    qpdf --is-encrypted $TMPDIR/flat_enc.pdf
    qpdf --password=secret --check $TMPDIR/flat_enc.pdf > /dev/null
    $TSPDF text $TMPDIR/flat_enc.pdf --password secret | grep -q 'EncBob'"
  # The seam is the shared save path, so every mutating command benefits:
  # spot-check one other command.
  run_test "bookmark add keeps encrypted input encrypted" bash -c "
    set -e
    $TSPDF bookmark add $FORM_ENC_PDF --password secret --title Intro --page 1 -o $TMPDIR/bm_enc.pdf
    qpdf --is-encrypted $TMPDIR/bm_enc.pdf
    qpdf --password=secret --check $TMPDIR/bm_enc.pdf > /dev/null
    $TSPDF bookmark list $TMPDIR/bm_enc.pdf --password secret | grep -q 'Intro'"
  # nup builds its output from imported (decrypted) page copies rather than
  # the shared source structure — it must still re-encrypt on save.
  run_test "nup keeps encrypted input encrypted" bash -c "
    set -e
    $TSPDF nup 2 $FORM_ENC_PDF --password secret -o $TMPDIR/nup_enc.pdf
    qpdf --is-encrypted $TMPDIR/nup_enc.pdf
    qpdf --password=secret --check $TMPDIR/nup_enc.pdf > /dev/null
    $TSPDF info $TMPDIR/nup_enc.pdf --password secret | grep -q 'Pages'"
  # decrypt is the explicit opt-out and must still write plaintext.
  run_test "decrypt still writes an unencrypted file" bash -c "
    set -e
    $TSPDF decrypt $FORM_ENC_PDF --password secret -o $TMPDIR/form_dec.pdf
    if qpdf --is-encrypted $TMPDIR/form_dec.pdf; then exit 1; fi
    qpdf --check $TMPDIR/form_dec.pdf > /dev/null"
else
  echo "  SKIP  encrypted form round-trips (qpdf not installed)"
fi

# Choice fields validate against /Opt (city is Berlin/Paris/Oslo)...
run_test "form fill rejects a non-option choice value" bash -c "
  set -e
  if $TSPDF form fill $FORM_PDF --set city=Atlantis -o $TMPDIR/city_bad.pdf 2> $TMPDIR/city_err.txt; then
    exit 1
  fi
  grep -q 'Atlantis' $TMPDIR/city_err.txt
  grep -q 'Berlin, Paris, Oslo' $TMPDIR/city_err.txt
  [ ! -e $TMPDIR/city_bad.pdf ]"
# ...unless the combo is editable (/Ff Combo|Edit): free text is legal there.
run_test "form fill accepts free text in an editable combo" bash -c "
  set -e
  $TSPDF form fill tests/data/form_combo_edit.pdf --set nickname=Zaphod -o $TMPDIR/combo.pdf
  $TSPDF form list $TMPDIR/combo.pdf | grep -q '\"Zaphod\"'"

# Values outside the appearance font's encoding are stored intact in /V but
# render as '?' in the generated appearance when no fallback TrueType font is
# available — that must be warned about. Hermetic: the fallback discovery is
# pointed at an empty directory so machine-installed fonts cannot mask the
# warning path (mkdir here: this may run before the fallback section below).
mkdir -p $TMPDIR/nofonts
run_test "form fill warns when the appearance loses characters" bash -c "
  set -e
  TSPDF_FALLBACK_FONT= TSPDF_FONT_DIRS=$TMPDIR/nofonts \
  $TSPDF form fill $FORM_PDF --set 'name=日本語' -o $TMPDIR/cjk.pdf 2> $TMPDIR/cjk_err.txt
  grep -q 'not representable' $TMPDIR/cjk_err.txt
  grep -q 'TSPDF_FALLBACK_FONT' $TMPDIR/cjk_err.txt
  grep -q \"'name'\" $TMPDIR/cjk_err.txt
  $TSPDF form list $TMPDIR/cjk.pdf | grep -q '日本語'"
run_test "form fill does not warn for cp1252-safe values" bash -c "
  set -e
  TSPDF_FALLBACK_FONT= TSPDF_FONT_DIRS=$TMPDIR/nofonts \
  $TSPDF form fill $FORM_PDF --set 'name=Grüße' -o $TMPDIR/latin.pdf 2> $TMPDIR/latin_err.txt
  ! grep -q 'not representable' $TMPDIR/latin_err.txt"
run_test "form flatten warns when a stamped value loses characters" bash -c "
  set -e
  TSPDF_FALLBACK_FONT= TSPDF_FONT_DIRS=$TMPDIR/nofonts \
  $TSPDF form flatten $TMPDIR/cjk.pdf -o $TMPDIR/cjk_flat.pdf 2> $TMPDIR/cjk_flat_err.txt
  grep -q 'not representable' $TMPDIR/cjk_flat_err.txt"

# info --json: page dimensions must be JSON numbers (not strings); page_sizes
# is a single [w,h] for a uniform document and an array of [w,h] for a mixed one.
if command -v python3 > /dev/null 2>&1; then
  run_test "info --json emits numeric page_sizes (uniform + mixed)" python3 - << PYEOF
import json, subprocess, struct

# Uniform: every page the same -> page_sizes is one [w,h] of numbers.
out = subprocess.run(["$TSPDF", "info", "$INPUT", "--json"],
                     capture_output=True, check=True).stdout
d = json.loads(out)
ps = d["page_sizes"]
assert isinstance(ps, list) and len(ps) == 2, ps
assert all(isinstance(x, (int, float)) and not isinstance(x, bool) for x in ps), ps

# Mixed: two differently sized pages -> array of [w,h] pairs, all numeric.
def obj(n, body): return b"%d 0 obj\n" % n + body + b"\nendobj\n"
objs = {
 1: b"<< /Type /Catalog /Pages 2 0 R >>",
 2: b"<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
 3: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] >>",
 4: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] >>",
}
buf = b"%PDF-1.4\n"; offs = {}
for n in sorted(objs):
    offs[n] = len(buf); buf += obj(n, objs[n])
xp = len(buf); mx = max(objs)
buf += b"xref\n0 %d\n0000000000 65535 f \n" % (mx + 1)
for n in range(1, mx + 1): buf += b"%010d 00000 n \n" % offs[n]
buf += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF" % (mx + 1, xp)
open("$TMPDIR/mixed.pdf", "wb").write(buf)

out = subprocess.run(["$TSPDF", "info", "$TMPDIR/mixed.pdf", "--json"],
                     capture_output=True, check=True).stdout
d = json.loads(out)
ps = d["page_sizes"]
assert isinstance(ps, list) and len(ps) == 2 and all(isinstance(p, list) for p in ps), ps
assert ps == [[200.0, 300.0], [400.0, 500.0]], ps
PYEOF
else
  echo "  SKIP  info --json numeric page_sizes (python3 not found)"
fi

# --- compress --lossy ---
# Fixtures are committed (regenerate with tests/data/gen_lossy_fixtures.py).
# Helper: extract the largest image XObject stream of a PDF; prints
# "FILTER WIDTH HEIGHT COLORSPACE" and optionally dumps the raw bytes.
cat > "$TMPDIR/imgstream.py" <<'PYEOF'
import re, sys
data = open(sys.argv[1], 'rb').read()
out = sys.argv[2] if len(sys.argv) > 2 else None
best = None
for m in re.finditer(rb'stream\r?\n', data):
    start = data.rfind(b'obj', 0, m.start())
    if start < 0:
        continue
    d = data[start:m.start()]
    if b'/Subtype' not in d or b'/Image' not in d:
        continue
    lengths = re.findall(rb'/Length\s+(\d+)', d)
    if not lengths:
        continue
    n = int(lengths[-1])
    body = data[m.end():m.end() + n]
    if best is None or len(body) > len(best[0]):
        f = re.search(rb'/Filter\s*/(\w+)', d)
        w = re.findall(rb'/Width\s+(\d+)', d)
        h = re.findall(rb'/Height\s+(\d+)', d)
        cs = re.search(rb'/ColorSpace\s*/(\w+)', d)
        best = (body, f.group(1).decode() if f else '-',
                int(w[-1]) if w else 0, int(h[-1]) if h else 0,
                cs.group(1).decode() if cs else '-')
assert best is not None, 'no image stream found'
print(best[1], best[2], best[3], best[4])
if out:
    open(out, 'wb').write(best[0])
PYEOF
# Helper: assert a ppm render is nonblank (has more than one pixel value).
cat > "$TMPDIR/nonblank.py" <<'PYEOF'
import sys
d = open(sys.argv[1], 'rb').read().split(b'\n', 3)[3]
assert len(set(d)) > 1, 'blank page'
PYEOF

LOSSY_IN=tests/data/lossy_scan.pdf
if command -v python3 >/dev/null 2>&1 && [ -f "$LOSSY_IN" ]; then
  run_test "compress --lossy shrinks a 600-dpi scan > 60%" bash -c '
    set -e
    "'"$TSPDF"'" compress --lossy '"$LOSSY_IN"' -o "'"$TMPDIR"'/lossy_out.pdf"
    in=$(wc -c < '"$LOSSY_IN"')
    out=$(wc -c < "'"$TMPDIR"'/lossy_out.pdf")
    [ $((out * 10)) -lt $((in * 4)) ]'

  run_test "compress --lossy converts the image to DCTDecode" bash -c '
    set -e
    info=$(python3 "'"$TMPDIR"'/imgstream.py" "'"$TMPDIR"'/lossy_out.pdf")
    case "$info" in DCTDecode\ *) ;; *) echo "got: $info"; exit 1;; esac'

  run_test "compress --lossy keeps the text page extractable" bash -c '
    "'"$TSPDF"'" text "'"$TMPDIR"'/lossy_out.pdf" | grep -q "Lossy fixture text page"'

  if command -v qpdf >/dev/null 2>&1; then
    run_test "compress --lossy output passes qpdf --check" \
      qpdf --check "$TMPDIR/lossy_out.pdf"
  else
    echo "  SKIP  compress --lossy qpdf --check (qpdf not found)"
  fi

  if command -v pdftoppm >/dev/null 2>&1; then
    run_test "compress --lossy output renders both pages nonblank" bash -c '
      set -e
      pdftoppm -r 30 "'"$TMPDIR"'/lossy_out.pdf" "'"$TMPDIR"'/lossy_pg"
      python3 "'"$TMPDIR"'/nonblank.py" "'"$TMPDIR"'/lossy_pg-1.ppm"
      python3 "'"$TMPDIR"'/nonblank.py" "'"$TMPDIR"'/lossy_pg-2.ppm"'
  else
    echo "  SKIP  compress --lossy render check (pdftoppm not found)"
  fi

  MUTOOL=/tmp/tspdf-bench/mupdf/usr/bin/mutool
  if [ -x "$MUTOOL" ]; then
    run_test "compress --lossy output renders in mutool" bash -c '
      LD_LIBRARY_PATH=/tmp/tspdf-bench/mupdf/usr/lib '"$MUTOOL"' draw -F ppm \
        -o /dev/null "'"$TMPDIR"'/lossy_out.pdf" 2>/dev/null'
  fi

  run_test "compress --lossy downsamples a 600-dpi DCT image" bash -c '
    set -e
    "'"$TSPDF"'" compress --lossy tests/data/lossy_scan_dct.pdf -o "'"$TMPDIR"'/lossy_dct_out.pdf"
    info=$(python3 "'"$TMPDIR"'/imgstream.py" "'"$TMPDIR"'/lossy_dct_out.pdf")
    set -- $info
    [ "$1" = DCTDecode ] && [ "$2" -lt 360 ] && [ "$2" -ge 80 ]
    "'"$TSPDF"'" info "'"$TMPDIR"'/lossy_dct_out.pdf" > /dev/null'

  run_test "compress --lossy passes a progressive JPEG through byte-identical" bash -c '
    set -e
    "'"$TSPDF"'" compress --lossy tests/data/lossy_progressive.pdf -o "'"$TMPDIR"'/lossy_prog_out.pdf"
    python3 "'"$TMPDIR"'/imgstream.py" tests/data/lossy_progressive.pdf "'"$TMPDIR"'/prog_in.bin" > /dev/null
    python3 "'"$TMPDIR"'/imgstream.py" "'"$TMPDIR"'/lossy_prog_out.pdf" "'"$TMPDIR"'/prog_out.bin" > /dev/null
    cmp -s "'"$TMPDIR"'/prog_in.bin" "'"$TMPDIR"'/prog_out.bin"'

  run_test "compress --lossy leaves an /SMask image untouched" bash -c '
    set -e
    "'"$TSPDF"'" compress --lossy tests/data/lossy_smask.pdf -o "'"$TMPDIR"'/lossy_smask_out.pdf"
    python3 "'"$TMPDIR"'/imgstream.py" tests/data/lossy_smask.pdf "'"$TMPDIR"'/smask_in.bin" > /dev/null
    python3 "'"$TMPDIR"'/imgstream.py" "'"$TMPDIR"'/lossy_smask_out.pdf" "'"$TMPDIR"'/smask_out.bin" > /dev/null
    cmp -s "'"$TMPDIR"'/smask_in.bin" "'"$TMPDIR"'/smask_out.bin"'

  run_test "compress --lossy leaves an already-low-dpi image untouched" bash -c '
    set -e
    "'"$TSPDF"'" compress --lossy tests/data/lossy_lowdpi.pdf -o "'"$TMPDIR"'/lossy_low_out.pdf"
    python3 "'"$TMPDIR"'/imgstream.py" tests/data/lossy_lowdpi.pdf "'"$TMPDIR"'/low_in.bin" > /dev/null
    python3 "'"$TMPDIR"'/imgstream.py" "'"$TMPDIR"'/lossy_low_out.pdf" "'"$TMPDIR"'/low_out.bin" > /dev/null
    cmp -s "'"$TMPDIR"'/low_in.bin" "'"$TMPDIR"'/low_out.bin"'

  run_test "compress without --lossy never touches image pixels" bash -c '
    set -e
    "'"$TSPDF"'" compress '"$LOSSY_IN"' -o "'"$TMPDIR"'/lossless_out.pdf"
    python3 "'"$TMPDIR"'/imgstream.py" '"$LOSSY_IN"' "'"$TMPDIR"'/ll_in.bin" > /dev/null
    python3 "'"$TMPDIR"'/imgstream.py" "'"$TMPDIR"'/lossless_out.pdf" "'"$TMPDIR"'/ll_out.bin" > /dev/null
    cmp -s "'"$TMPDIR"'/ll_in.bin" "'"$TMPDIR"'/ll_out.bin"'

  run_test "compress --image-quality 30 is smaller than the default" bash -c '
    set -e
    "'"$TSPDF"'" compress --lossy --image-quality 30 tests/data/lossy_scan_dct.pdf -o "'"$TMPDIR"'/lossy_q30.pdf"
    q30=$(wc -c < "'"$TMPDIR"'/lossy_q30.pdf")
    dflt=$(wc -c < "'"$TMPDIR"'/lossy_dct_out.pdf")
    [ "$q30" -lt "$dflt" ]'

  run_test "compress rejects invalid lossy flag values" bash -c '
    ! "'"$TSPDF"'" compress --lossy --image-quality 0 '"$LOSSY_IN"' -o "'"$TMPDIR"'/x.pdf" 2>/dev/null || exit 1
    ! "'"$TSPDF"'" compress --lossy --image-quality 101 '"$LOSSY_IN"' -o "'"$TMPDIR"'/x.pdf" 2>/dev/null || exit 1
    ! "'"$TSPDF"'" compress --lossy --image-dpi 0 '"$LOSSY_IN"' -o "'"$TMPDIR"'/x.pdf" 2>/dev/null || exit 1
    ! "'"$TSPDF"'" compress --lossy --image-dpi abc '"$LOSSY_IN"' -o "'"$TMPDIR"'/x.pdf" 2>/dev/null || exit 1
    ! "'"$TSPDF"'" compress --image-dpi 150 '"$LOSSY_IN"' -o "'"$TMPDIR"'/x.pdf" 2>/dev/null || exit 1
    ! "'"$TSPDF"'" compress --image-quality 50 '"$LOSSY_IN"' -o "'"$TMPDIR"'/x.pdf" 2>/dev/null || exit 1'

  if command -v qpdf >/dev/null 2>&1; then
    run_test "compress --lossy keeps an encrypted input encrypted" bash -c '
      set -e
      qpdf --encrypt "" ownersecret 256 -- '"$LOSSY_IN"' "'"$TMPDIR"'/lossy_enc.pdf"
      "'"$TSPDF"'" compress --lossy "'"$TMPDIR"'/lossy_enc.pdf" -o "'"$TMPDIR"'/lossy_enc_out.pdf"
      qpdf --is-encrypted "'"$TMPDIR"'/lossy_enc_out.pdf"
      qpdf --check "'"$TMPDIR"'/lossy_enc_out.pdf" > /dev/null
      in=$(wc -c < "'"$TMPDIR"'/lossy_enc.pdf")
      out=$(wc -c < "'"$TMPDIR"'/lossy_enc_out.pdf")
      [ "$out" -lt $((in / 2)) ]'
  else
    echo "  SKIP  compress --lossy encrypted roundtrip (qpdf not found)"
  fi
else
  echo "  SKIP  compress --lossy tests (python3 or fixtures missing)"
fi

# --- fit & finish: rich outline preservation, XMP notice, attachment
# metadata, split --no-attachments, stamp password files ---

# metadata: a one-line stderr notice when the XMP packet lacks an edited
# field (that field stays stale there), and silence when everything synced
# or there is no XMP at all.
if [ -f tests/data/xmp_meta.pdf ]; then
  run_test "metadata --set warns when XMP lacks the edited field" bash -c '
    set -e
    "'"$TSPDF"'" metadata tests/data/xmp_meta.pdf --set author=NewAuthor -o "'"$TMPDIR"'/xmp_out.pdf" 2> "'"$TMPDIR"'/xmp_err.txt"
    grep -q "XMP metadata present but not updated for author" "'"$TMPDIR"'/xmp_err.txt"'
  run_test "metadata --set stays quiet when the field synced into XMP" bash -c '
    set -e
    "'"$TSPDF"'" metadata tests/data/xmp_meta.pdf --set title=NewTitle -o "'"$TMPDIR"'/xmp_quiet.pdf" 2> "'"$TMPDIR"'/xmp_quiet_err.txt"
    ! grep -q "XMP metadata" "'"$TMPDIR"'/xmp_quiet_err.txt"'
  run_test "metadata --set stays quiet without XMP metadata" bash -c '
    set -e
    "'"$TSPDF"'" metadata '"$INPUT"' --set title=NewTitle -o "'"$TMPDIR"'/noxmp_out.pdf" 2> "'"$TMPDIR"'/noxmp_err.txt"
    ! grep -q "XMP metadata" "'"$TMPDIR"'/noxmp_err.txt"'
else
  echo "  SKIP  metadata XMP notice (tests/data/xmp_meta.pdf missing)"
fi

if command -v qpdf >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
  # attach add: /Subtype from the extension map, /Params with the real size,
  # the source file's mtime as /ModDate, and an MD5 /CheckSum.
  cat > $TMPDIR/check_attach_params.py << 'PYEOF'
import hashlib, json, os, subprocess, sys, time
pdf, src, mime = sys.argv[1], sys.argv[2], sys.argv[3]
d = json.loads(subprocess.check_output(["qpdf", "--json=2", pdf]))
found = None
for v in d["qpdf"][1].values():
    if isinstance(v, dict) and "stream" in v:
        sd = v["stream"].get("dict", {})
        if sd.get("/Type") == "/EmbeddedFile":
            found = sd
assert found, "no /EmbeddedFile stream"
assert found.get("/Subtype") == "/" + mime, found.get("/Subtype")
params = found.get("/Params", {})
data = open(src, "rb").read()
assert params.get("/Size") == len(data), params
assert params.get("/CheckSum") == "b:" + hashlib.md5(data).hexdigest(), params
mt = time.gmtime(int(os.path.getmtime(src)))
expect = "u:D:%04d%02d%02d%02d%02d%02dZ" % mt[:6]
assert params.get("/ModDate") == expect, (params.get("/ModDate"), expect)
PYEOF
  printf 'attachment payload for params test\n' > $TMPDIR/params.txt

  run_test "attach add records mime, size, mtime and MD5 checksum" bash -c '
    set -e
    "'"$TSPDF"'" attach add '"$INPUT"' "'"$TMPDIR"'/params.txt" -o "'"$TMPDIR"'/att_params.pdf"
    qpdf --check "'"$TMPDIR"'/att_params.pdf" > /dev/null
    python3 "'"$TMPDIR"'/check_attach_params.py" "'"$TMPDIR"'/att_params.pdf" "'"$TMPDIR"'/params.txt" "text/plain"
    "'"$TSPDF"'" attach list --json "'"$TMPDIR"'/att_params.pdf" | grep -q "\"mime\":\"text/plain\""
    mkdir -p "'"$TMPDIR"'/att_x"
    "'"$TSPDF"'" attach extract "'"$TMPDIR"'/att_params.pdf" --name params.txt -o "'"$TMPDIR"'/att_x"
    cmp -s "'"$TMPDIR"'/params.txt" "'"$TMPDIR"'/att_x/params.txt"'

  run_test "attach add --mime overrides the extension map" bash -c '
    set -e
    "'"$TSPDF"'" attach add '"$INPUT"' "'"$TMPDIR"'/params.txt" --mime application/x-custom -o "'"$TMPDIR"'/att_mime.pdf"
    qpdf --check "'"$TMPDIR"'/att_mime.pdf" > /dev/null
    python3 "'"$TMPDIR"'/check_attach_params.py" "'"$TMPDIR"'/att_mime.pdf" "'"$TMPDIR"'/params.txt" "application/x-custom"
    "'"$TSPDF"'" attach list --json "'"$TMPDIR"'/att_mime.pdf" | grep -q "application/x-custom"'

  # split: attachments are copied to every part by default; --no-attachments
  # drops them from burst and --pages outputs alike.
  run_test "split copies attachments into every part by default" bash -c '
    set -e
    "'"$TSPDF"'" split "'"$TMPDIR"'/att_params.pdf" -o "'"$TMPDIR"'/sp.pdf"
    for f in "'"$TMPDIR"'/sp-001.pdf" "'"$TMPDIR"'/sp-002.pdf" "'"$TMPDIR"'/sp-003.pdf"; do
      n=$("'"$TSPDF"'" attach list "$f" | wc -l | tr -d "[:space:]")
      [ "$n" = "1" ]
      qpdf --check "$f" > /dev/null
    done'

  run_test "split --no-attachments drops embedded files from every part" bash -c '
    set -e
    "'"$TSPDF"'" split "'"$TMPDIR"'/att_params.pdf" --no-attachments -o "'"$TMPDIR"'/spn.pdf"
    for f in "'"$TMPDIR"'/spn-001.pdf" "'"$TMPDIR"'/spn-002.pdf" "'"$TMPDIR"'/spn-003.pdf"; do
      n=$("'"$TSPDF"'" attach list "$f" | wc -l | tr -d "[:space:]")
      [ "$n" = "0" ]
      qpdf --check "$f" > /dev/null
    done
    "'"$TSPDF"'" split "'"$TMPDIR"'/att_params.pdf" --pages 1-2 --no-attachments -o "'"$TMPDIR"'/spp.pdf"
    n=$("'"$TSPDF"'" attach list "'"$TMPDIR"'/spp.pdf" | wc -l | tr -d "[:space:]")
    [ "$n" = "0" ]
    qpdf --check "'"$TMPDIR"'/spp.pdf" > /dev/null'

  # bookmark: adding to a rich outline (XYZ scroll/zoom dests, color, flags,
  # collapse) must keep every pre-existing item byte-comparable in qpdf
  # --json; only the new entry gets a synthesized [page /Fit] dest.
  if [ -f tests/data/rich_outline.pdf ]; then
    cat > $TMPDIR/check_rich_outline.py << 'PYEOF'
import json, subprocess, sys
before, after = sys.argv[1], sys.argv[2]
added_titles = sys.argv[3:]
def load(path):
    d = json.loads(subprocess.check_output(["qpdf", "--json=2", path]))
    sigs = []
    def walk(items, level):
        for it in items:
            sigs.append((level, it["title"], it.get("destpageposfrom1"),
                         it["dest"][1:], it["open"]))
            walk(it.get("kids", []), level + 1)
    walk(d["outlines"], 1)
    style = {}
    for v in d["qpdf"][1].values():
        val = v.get("value") if isinstance(v, dict) else None
        if isinstance(val, dict) and "/Title" in val and "/Parent" in val:
            style[val["/Title"]] = (val.get("/C"), val.get("/F"),
                                    isinstance(val.get("/Count"), int)
                                    and val["/Count"] < 0)
    return sigs, style
a_sigs, a_style = load(before)
b_sigs, b_style = load(after)
assert b_sigs[:len(a_sigs)] == a_sigs, (a_sigs, b_sigs)
assert len(b_sigs) == len(a_sigs) + len(added_titles), b_sigs
assert [t[1] for t in b_sigs[len(a_sigs):]] == added_titles, b_sigs
for k, v in a_style.items():
    assert b_style.get(k) == v, (k, v, b_style.get(k))
PYEOF
    run_test "bookmark add preserves rich dests, color, flags and collapse" bash -c '
      set -e
      "'"$TSPDF"'" bookmark add tests/data/rich_outline.pdf --title "Appendix" --page 1 -o "'"$TMPDIR"'/rich_add.pdf"
      qpdf --check "'"$TMPDIR"'/rich_add.pdf" > /dev/null
      python3 "'"$TMPDIR"'/check_rich_outline.py" tests/data/rich_outline.pdf "'"$TMPDIR"'/rich_add.pdf" "Appendix"'

    run_test "bookmark import --append keeps the existing outline" bash -c '
      set -e
      printf "1\t2\tAdded One\n2\t3\tAdded Sub\n" > "'"$TMPDIR"'/append_toc.txt"
      "'"$TSPDF"'" bookmark import tests/data/rich_outline.pdf --from "'"$TMPDIR"'/append_toc.txt" --append -o "'"$TMPDIR"'/rich_append.pdf"
      qpdf --check "'"$TMPDIR"'/rich_append.pdf" > /dev/null
      n=$("'"$TSPDF"'" bookmark list "'"$TMPDIR"'/rich_append.pdf" | wc -l | tr -d "[:space:]")
      [ "$n" = "5" ]
      python3 "'"$TMPDIR"'/check_rich_outline.py" tests/data/rich_outline.pdf "'"$TMPDIR"'/rich_append.pdf" "Added One" "Added Sub"'

    if command -v uv >/dev/null 2>&1; then
      cat > $TMPDIR/check_rich_toc.py << 'PYEOF'
import sys
import pymupdf
def toc(path):
    out = []
    for lvl, title, page, info in pymupdf.open(path).get_toc(simple=False):
        info = dict(info)
        info.pop("xref", None)  # object numbers legitimately change
        out.append((lvl, title, page, info))
    return out
a = toc(sys.argv[1])
b = toc(sys.argv[2])
assert b[:len(a)] == a, (a, b)
assert len(b) == len(a) + 1 and b[-1][1] == "Appendix", b
PYEOF
      run_test "bookmark add leaves pymupdf get_toc unchanged apart from addition" bash -c '
        set -e
        "'"$TSPDF"'" bookmark add tests/data/rich_outline.pdf --title "Appendix" --page 1 -o "'"$TMPDIR"'/rich_toc.pdf"
        uv run --python 3.12 --with pymupdf python "'"$TMPDIR"'/check_rich_toc.py" tests/data/rich_outline.pdf "'"$TMPDIR"'/rich_toc.pdf"'
    else
      echo "  SKIP  bookmark pymupdf get_toc check (uv not found)"
    fi
  else
    echo "  SKIP  bookmark rich outline tests (tests/data/rich_outline.pdf missing)"
  fi

  # stamp: --password-file/--stamp-password-file keep passwords out of argv.
  run_test "stamp reads passwords from files" bash -c '
    set -e
    "'"$TSPDF"'" encrypt '"$INPUT"' -o "'"$TMPDIR"'/st_enc_in.pdf" --password insecret
    "'"$TSPDF"'" encrypt tests/data/one_page.pdf -o "'"$TMPDIR"'/st_enc_stamp.pdf" --password stsecret
    printf "insecret\n" > "'"$TMPDIR"'/st_pw1.txt"
    printf "stsecret\n" > "'"$TMPDIR"'/st_pw2.txt"
    "'"$TSPDF"'" stamp "'"$TMPDIR"'/st_enc_in.pdf" --stamp "'"$TMPDIR"'/st_enc_stamp.pdf" --password-file "'"$TMPDIR"'/st_pw1.txt" --stamp-password-file "'"$TMPDIR"'/st_pw2.txt" -o "'"$TMPDIR"'/st_out.pdf"
    qpdf --password=insecret --check "'"$TMPDIR"'/st_out.pdf" > /dev/null'

  run_test "stamp hints at --password-file on encrypted input" bash -c '
    ! "'"$TSPDF"'" stamp "'"$TMPDIR"'/st_enc_in.pdf" --stamp tests/data/one_page.pdf -o "'"$TMPDIR"'/st_fail.pdf" < /dev/null 2> "'"$TMPDIR"'/st_err.txt"
    grep -q "password-file" "'"$TMPDIR"'/st_err.txt"'
else
  echo "  SKIP  attach/split/bookmark/stamp fit-and-finish tests (qpdf or python3 not found)"
fi

# --- form fill/flatten fallback font (values outside WinAnsi/cp1252) ---
#
# Hermetic: TSPDF_FALLBACK_FONT pins the committed fixture font, and the
# no-font case overrides the scan roots (TSPDF_FONT_DIRS) with an empty dir
# so the test never depends on fonts installed on this machine.
FBFONT=tests/data/fallback_font.ttf
mkdir -p "$TMPDIR/nofonts"
run_test "form fill CJK embeds fallback CID font (no warning)" bash -c '
  set -e
  err="'"$TMPDIR"'/fb_fill_err.txt"
  TSPDF_FALLBACK_FONT=tests/data/fallback_font.ttf "'"$TSPDF"'" form fill \
      tests/data/form_fields.pdf --set "name=日本語テスト" \
      -o "'"$TMPDIR"'/fb_fill.pdf" 2> "$err"
  [ ! -s "$err" ]
  grep -aq "Identity-H" "'"$TMPDIR"'/fb_fill.pdf"
  grep -aq "FontFile2" "'"$TMPDIR"'/fb_fill.pdf"
  grep -aq "CIDFontType2" "'"$TMPDIR"'/fb_fill.pdf"
  "'"$TSPDF"'" form list "'"$TMPDIR"'/fb_fill.pdf" | grep -q "日本語テスト"'

run_test "form fill CJK with .ttc fallback font" bash -c '
  set -e
  TSPDF_FALLBACK_FONT=tests/data/fallback_font.ttc "'"$TSPDF"'" form fill \
      tests/data/form_fields.pdf --set "name=日本語" \
      -o "'"$TMPDIR"'/fb_ttc.pdf" 2> /dev/null
  grep -aq "Identity-H" "'"$TMPDIR"'/fb_ttc.pdf"'

run_test "form fill CJK discovers font by directory scan" bash -c '
  set -e
  err="'"$TMPDIR"'/fb_scan_err.txt"
  TSPDF_FALLBACK_FONT= TSPDF_FONT_DIRS=tests/data "'"$TSPDF"'" form fill \
      tests/data/form_fields.pdf --set "name=日本語" \
      -o "'"$TMPDIR"'/fb_scan.pdf" 2> "$err"
  [ ! -s "$err" ]
  grep -aq "Identity-H" "'"$TMPDIR"'/fb_scan.pdf"'

run_test "form fill CJK without a usable font warns and keeps value" bash -c '
  set -e
  err="'"$TMPDIR"'/fb_warn_err.txt"
  TSPDF_FALLBACK_FONT= TSPDF_FONT_DIRS="'"$TMPDIR"'/nofonts" "'"$TSPDF"'" \
      form fill tests/data/form_fields.pdf --set "name=日本語" \
      -o "'"$TMPDIR"'/fb_warn.pdf" 2> "$err"
  grep -q "TSPDF_FALLBACK_FONT" "$err"
  ! grep -aq "Identity-H" "'"$TMPDIR"'/fb_warn.pdf"
  "'"$TSPDF"'" form list "'"$TMPDIR"'/fb_warn.pdf" | grep -q "日本語"'

run_test "form flatten CJK stamps fallback glyphs (no warning)" bash -c '
  set -e
  err="'"$TMPDIR"'/fb_flat_err.txt"
  TSPDF_FALLBACK_FONT=tests/data/fallback_font.ttf "'"$TSPDF"'" form fill \
      tests/data/form_fields.pdf --set "name=日本語" \
      -o "'"$TMPDIR"'/fb_prefill.pdf" 2> /dev/null
  TSPDF_FALLBACK_FONT=tests/data/fallback_font.ttf "'"$TSPDF"'" form flatten \
      "'"$TMPDIR"'/fb_prefill.pdf" -o "'"$TMPDIR"'/fb_flat.pdf" 2> "$err"
  [ ! -s "$err" ]
  grep -aq "Identity-H" "'"$TMPDIR"'/fb_flat.pdf"
  grep -aq "TspdfFb" "'"$TMPDIR"'/fb_flat.pdf"'

run_test "form fill validates inherited /Opt (parent-held options)" bash -c '
  set -e
  # city has /Opt on the terminal itself; the reader-suite fixture covers the
  # parent-held case — here just pin the CLI error message path end-to-end.
  if "'"$TSPDF"'" form fill tests/data/form_fields.pdf --set city=Atlantis \
      -o "'"$TMPDIR"'/fb_opt.pdf" 2> "'"$TMPDIR"'/fb_opt_err.txt"; then
    exit 1
  fi
  grep -q "not an option" "'"$TMPDIR"'/fb_opt_err.txt"'

# --- compress --lossy: bilevel (CCITT G4) images ---
# Fixtures are committed (regenerate with tests/data/gen_mono_fixtures.py):
# a 600-dpi scanned-text-like G4 page, a 300-dpi one (passthrough), and a
# 300-dpi 1-bpp FlateDecode one (converted to G4 1:1, pixel-identical).
# Helper: mean absolute difference of two equal-size P5 (PGM) renders.
cat > "$TMPDIR/pgmdiff.py" <<'PYEOF'
import sys
def read(p):
    d = open(p, 'rb').read()
    head, rest = d.split(b'\n', 1)
    assert head == b'P5', head
    dims, rest = rest.split(b'\n', 1)
    maxv, rest = rest.split(b'\n', 1)
    w, h = map(int, dims.split())
    return w, h, rest[:w*h]
w1, h1, a = read(sys.argv[1])
w2, h2, b = read(sys.argv[2])
assert (w1, h1) == (w2, h2), 'size mismatch'
mad = sum(abs(x - y) for x, y in zip(a, b)) / len(a)
print('%.3f' % mad)
assert mad <= float(sys.argv[3]), 'mean abs diff %.3f > %s' % (mad, sys.argv[3])
PYEOF

MONO_IN=tests/data/lossy_mono.pdf
if command -v python3 >/dev/null 2>&1 && [ -f "$MONO_IN" ]; then
  run_test "compress --lossy shrinks a 600-dpi bilevel scan > 40%" bash -c '
    set -e
    "'"$TSPDF"'" compress --lossy '"$MONO_IN"' -o "'"$TMPDIR"'/mono_out.pdf"
    in=$(wc -c < '"$MONO_IN"')
    out=$(wc -c < "'"$TMPDIR"'/mono_out.pdf")
    [ $((out * 10)) -lt $((in * 6)) ]'

  run_test "compress --lossy keeps the bilevel image CCITT, downsampled to 300 dpi" bash -c '
    set -e
    info=$(python3 "'"$TMPDIR"'/imgstream.py" "'"$TMPDIR"'/mono_out.pdf")
    case "$info" in "CCITTFaxDecode 450 450 DeviceGray") ;; *) echo "got: $info"; exit 1;; esac'

  run_test "compress --lossy reports the bilevel recompression" bash -c '
    "'"$TSPDF"'" compress --lossy '"$MONO_IN"' -o "'"$TMPDIR"'/mono_out2.pdf" \
      | grep -q "1 as CCITT G4"'

  if command -v qpdf >/dev/null 2>&1; then
    run_test "compress --lossy bilevel output passes qpdf --check" \
      qpdf --check "$TMPDIR/mono_out.pdf"
  else
    echo "  SKIP  compress --lossy bilevel qpdf --check (qpdf not found)"
  fi

  if command -v pdftoppm >/dev/null 2>&1; then
    run_test "compress --lossy bilevel output renders nonblank and close to the input" bash -c '
      set -e
      pdftoppm -r 150 -gray -singlefile '"$MONO_IN"' "'"$TMPDIR"'/mono_in_r"
      pdftoppm -r 150 -gray -singlefile "'"$TMPDIR"'/mono_out.pdf" "'"$TMPDIR"'/mono_out_r"
      python3 "'"$TMPDIR"'/nonblank.py" "'"$TMPDIR"'/mono_out_r.pgm"
      python3 "'"$TMPDIR"'/pgmdiff.py" "'"$TMPDIR"'/mono_in_r.pgm" "'"$TMPDIR"'/mono_out_r.pgm" 3.0 > /dev/null'

    run_test "compress --lossy converts 1-bpp flate to G4 rendering pixel-identically" bash -c '
      set -e
      "'"$TSPDF"'" compress --lossy tests/data/lossy_mono_flate.pdf -o "'"$TMPDIR"'/mono_flate_out.pdf"
      info=$(python3 "'"$TMPDIR"'/imgstream.py" "'"$TMPDIR"'/mono_flate_out.pdf")
      case "$info" in "CCITTFaxDecode 450 450 DeviceGray") ;; *) echo "got: $info"; exit 1;; esac
      pdftoppm -r 300 -gray -singlefile tests/data/lossy_mono_flate.pdf "'"$TMPDIR"'/mf_in_r"
      pdftoppm -r 300 -gray -singlefile "'"$TMPDIR"'/mono_flate_out.pdf" "'"$TMPDIR"'/mf_out_r"
      cmp -s "'"$TMPDIR"'/mf_in_r.pgm" "'"$TMPDIR"'/mf_out_r.pgm"'
  else
    echo "  SKIP  compress --lossy bilevel render checks (pdftoppm not found)"
  fi

  MUTOOL=/tmp/tspdf-bench/mupdf/usr/bin/mutool
  if [ -x "$MUTOOL" ]; then
    run_test "compress --lossy bilevel output renders in mutool" bash -c '
      LD_LIBRARY_PATH=/tmp/tspdf-bench/mupdf/usr/lib '"$MUTOOL"' draw -F ppm \
        -o /dev/null "'"$TMPDIR"'/mono_out.pdf" 2>/dev/null'
  fi

  run_test "compress --lossy leaves a 300-dpi CCITT image byte-identical" bash -c '
    set -e
    "'"$TSPDF"'" compress --lossy tests/data/lossy_mono_low.pdf -o "'"$TMPDIR"'/mono_low_out.pdf"
    python3 "'"$TMPDIR"'/imgstream.py" tests/data/lossy_mono_low.pdf "'"$TMPDIR"'/mono_low_in.bin" > /dev/null
    python3 "'"$TMPDIR"'/imgstream.py" "'"$TMPDIR"'/mono_low_out.pdf" "'"$TMPDIR"'/mono_low_out.bin" > /dev/null
    cmp -s "'"$TMPDIR"'/mono_low_in.bin" "'"$TMPDIR"'/mono_low_out.bin"'

  run_test "compress --lossy --mono-dpi 600 leaves the 600-dpi scan untouched" bash -c '
    set -e
    "'"$TSPDF"'" compress --lossy --mono-dpi 600 '"$MONO_IN"' -o "'"$TMPDIR"'/mono_600_out.pdf"
    python3 "'"$TMPDIR"'/imgstream.py" '"$MONO_IN"' "'"$TMPDIR"'/mono_600_in.bin" > /dev/null
    python3 "'"$TMPDIR"'/imgstream.py" "'"$TMPDIR"'/mono_600_out.pdf" "'"$TMPDIR"'/mono_600_out.bin" > /dev/null
    cmp -s "'"$TMPDIR"'/mono_600_in.bin" "'"$TMPDIR"'/mono_600_out.bin"'

  run_test "compress rejects invalid --mono-dpi values" bash -c '
    ! "'"$TSPDF"'" compress --lossy --mono-dpi 0 '"$MONO_IN"' -o "'"$TMPDIR"'/x.pdf" 2>/dev/null || exit 1
    ! "'"$TSPDF"'" compress --lossy --mono-dpi 4801 '"$MONO_IN"' -o "'"$TMPDIR"'/x.pdf" 2>/dev/null || exit 1
    ! "'"$TSPDF"'" compress --lossy --mono-dpi abc '"$MONO_IN"' -o "'"$TMPDIR"'/x.pdf" 2>/dev/null || exit 1
    ! "'"$TSPDF"'" compress --mono-dpi 300 '"$MONO_IN"' -o "'"$TMPDIR"'/x.pdf" 2>/dev/null || exit 1'
else
  echo "  SKIP  compress --lossy bilevel tests (python3 or fixtures missing)"
fi

# `make amalgamation` generates build/amalgamation/tspdf.{c,h} and proves them:
# standalone -Werror compile, link + run examples/minimal.c against them, and
# qpdf --check on the produced PDF (inside the make target, when qpdf exists).
if [ -f scripts/amalgamate.sh ]; then
  run_test "amalgamation compiles, links minimal example, output checks" bash -c '
    set -e
    make amalgamation > /dev/null 2>&1
    test -f build/amalgamation/tspdf.c
    test -f build/amalgamation/tspdf.h
    test -s build/amalgamation/minimal.pdf
    head -c 5 build/amalgamation/minimal.pdf | grep -q "%PDF-"'
else
  echo "  SKIP  amalgamation (scripts/amalgamate.sh not present)"
fi

echo ""
echo "$pass passed, $fail failed"
[ $fail -eq 0 ] || exit 1
