#!/bin/sh
# Byte-identity oracle for R2 Task G (save-pipeline staging).
# Runs `tspdf compress` (plain + --lossy) on every tests/data/*.pdf, plus a
# plain save and an encrypted-roundtrip save, and records sha256 of each output
# into a manifest. Run once before the change, once after; diff the manifests.
#
# Usage: scripts/saveplan_byteident.sh <tspdf-binary> <out-manifest>
set -e
BIN="$1"
MANIFEST="$2"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

: > "$MANIFEST"

for f in tests/data/*.pdf; do
    name="$(basename "$f")"
    # plain compress
    if "$BIN" compress "$f" -o "$TMP/c.pdf" >/dev/null 2>&1; then
        h="$(sha256sum "$TMP/c.pdf" | cut -d' ' -f1)"
    else
        h="COMPRESS_FAIL"
    fi
    echo "compress        $name $h" >> "$MANIFEST"

    # lossy compress
    if "$BIN" compress --lossy "$f" -o "$TMP/l.pdf" >/dev/null 2>&1; then
        h="$(sha256sum "$TMP/l.pdf" | cut -d' ' -f1)"
    else
        h="LOSSY_FAIL"
    fi
    echo "compress-lossy  $name $h" >> "$MANIFEST"

    # plain save (metadata no-op write): use `tspdf metadata` set/get round via
    # rotate 0? Simplest neutral plain-save is `tspdf metadata --producer`.
    # Use `tspdf pagenum`? Keep neutral: reserialize via `tspdf metadata`.
done

# Plain save (reserialize via standard path): rotate 90 exercises the standard
# re-serialization path without touching /Info timestamps.
if "$BIN" rotate tests/data/three_pages.pdf --angle 90 -o "$TMP/plain.pdf" >/dev/null 2>&1; then
    h="$(sha256sum "$TMP/plain.pdf" | cut -d' ' -f1)"
else
    h="PLAIN_FAIL"
fi
echo "plain-save      three_pages.pdf $h" >> "$MANIFEST"

# Encrypted round-trip. Encryption uses a fresh random file key and per-object
# IVs, so encrypted bytes are non-deterministic run-to-run and cannot be hashed
# directly. Instead: reserialize a FIXED encrypted fixture through the encrypted
# save path, then decrypt it, and hash the decrypted plaintext with the (random,
# source-carried) /ID line normalized out. This isolates what the encrypted
# writer emits (object set, dict shapes, /Length) from the crypto randomness.
# The fixture is supplied by $3 so both the before- and after-runs reserialize
# the same encrypted input.
ENCFIX="${3:-}"
if [ -n "$ENCFIX" ] && [ -f "$ENCFIX" ]; then
    if "$BIN" rotate "$ENCFIX" --password pw --angle 90 -o "$TMP/enc2.pdf" >/dev/null 2>&1 &&
       "$BIN" decrypt "$TMP/enc2.pdf" --password pw -o "$TMP/enc2_plain.pdf" >/dev/null 2>&1; then
        # Drop the /ID array line (random, carried from the source) before hashing.
        h2="$(grep -av '/ID' "$TMP/enc2_plain.pdf" | sha256sum | cut -d' ' -f1)"
    else
        h2="ENC_RESAVE_FAIL"
    fi
    echo "encrypt-resave-plaintext(no-ID) $(basename "$ENCFIX") $h2" >> "$MANIFEST"
fi

echo "wrote $MANIFEST"
