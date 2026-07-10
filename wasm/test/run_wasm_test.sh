#!/bin/bash
# wasm end-to-end gate: generate fixtures with the native CLI, run every wasm
# operation under node (wasm/test/wasm-test.js), then validate every produced
# PDF with qpdf --check. Requires: node, qpdf, a built native CLI, and a built
# wasm module (make wasm-test handles the ordering).
set -e

OUT=build/wasm-test
mkdir -p "$OUT"
rm -f "$OUT"/out_*.pdf

# Fixtures: two structurally different PDFs from the native CLI (text+list
# layout vs. QR code image content).
printf '# tspdf wasm fixture\n\nParagraph one for the wasm gate.\n\n- item a\n- item b\n' > "$OUT/fixture_a.md"
./build/tspdf md2pdf "$OUT/fixture_a.md" -o "$OUT/fixture_a.pdf"
./build/tspdf qrcode "https://example.com/tspdf-wasm" -o "$OUT/fixture_b.pdf" --title "wasm fixture"

node wasm/test/wasm-test.js "$OUT/fixture_a.pdf" "$OUT/fixture_b.pdf" "$OUT"

# Demo backend gate: lay the browser backend out next to the module the way
# the demo site ships it (so its relative imports resolve) and drive it under
# node — covers the /api contract and merge ordering for >= 11 uploads.
cp wasm/dist/tspdf.js wasm/dist/tspdf.wasm wasm/tspdf-api.js wasm/demo/wasm-backend.js "$OUT"
printf '{ "type": "module" }\n' > "$OUT/package.json"
node wasm/test/wasm-backend-test.js "$OUT/fixture_a.pdf" "$OUT/fixture_b.pdf" "$OUT"

# qpdf conformance gate over every wasm-produced output. The encrypted
# outputs need their password to be checkable; everything else (including
# out_decrypted.pdf, the unlock result) must open WITHOUT one.
fail=0
for f in "$OUT"/out_*.pdf; do
    case "$f" in
        *out_encrypted.pdf | *out_resaved_encrypted.pdf)
            set -- qpdf --password=wasmpw --check "$f" ;;
        *)  set -- qpdf --check "$f" ;;
    esac
    if "$@" > /dev/null 2>&1; then
        echo "  qpdf --check OK: $f"
    else
        echo "  qpdf --check FAILED: $f"
        "$@" || true
        fail=1
    fi
done
[ "$fail" -eq 0 ] || exit 1
echo "wasm-test: all outputs pass qpdf --check"
