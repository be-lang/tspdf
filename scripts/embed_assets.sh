#!/bin/bash
set -e
OUT="cli/assets.h"
# Provide an explicit template: BSD/macOS mktemp requires one, GNU mktemp
# accepts it too.
TMPFILE=$(mktemp "${TMPDIR:-/tmp}/tspdf_assets.XXXXXX")

echo "// Auto-generated from web/ — do not edit" > "$TMPFILE"
echo "#ifndef TSPDF_ASSETS_H" >> "$TMPFILE"
echo "#define TSPDF_ASSETS_H" >> "$TMPFILE"
echo "#include <stddef.h>" >> "$TMPFILE"

embed_file() {
    local path="$1"
    local varname="$2"
    echo "" >> "$TMPFILE"
    echo "static const unsigned char ${varname}[] = {" >> "$TMPFILE"
    # Feed od's hex bytes through awk so the output layout (12 bytes per line)
    # is identical regardless of how GNU vs BSD od spaces/wraps its columns.
    # This keeps the generated header byte-for-byte reproducible across Linux
    # and macOS.
    od -An -v -tx1 < "$path" | awk '
        { for (i = 1; i <= NF; i++) { printf "0x%s,", $i; if (++n % 12 == 0) printf "\n" } }
        END { if (n % 12 != 0) printf "\n" }
    ' >> "$TMPFILE"
    echo "};" >> "$TMPFILE"
    echo "static const size_t ${varname}_len = sizeof(${varname});" >> "$TMPFILE"
}

# Core assets
embed_file "web/templates/base.html" "asset_base_html"
embed_file "web/templates/index.html" "asset_index_html"
embed_file "web/static/style.css" "asset_style_css"
embed_file "web/static/app.js" "asset_app_js"

# Tool templates.
# Portable read loop instead of bash-4 `mapfile` so this runs on macOS's stock
# bash 3.2. Feeding via a here-string keeps the loop in the current shell (no
# subshell), and IFS= / read -r preserve exact filenames.
TOOL_TEMPLATES=$(find web/templates/tools -maxdepth 1 -type f -name '*.html' | LC_ALL=C sort)
while IFS= read -r f; do
    [ -n "$f" ] || continue
    # Convert filename to variable name: web/templates/tools/merge.html -> asset_tool_merge
    base=$(basename "$f" .html)
    varname="asset_tool_$(echo "$base" | tr '-' '_')"
    embed_file "$f" "$varname"
done <<EOF
$TOOL_TEMPLATES
EOF

echo "" >> "$TMPFILE"
echo "#endif" >> "$TMPFILE"

mv "$TMPFILE" "$OUT"
echo "Generated $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes)"
