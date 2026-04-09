#!/bin/bash
set -e
OUT="cli/assets.h"
TMPFILE=$(mktemp)

echo "// Auto-generated from web/ — do not edit" > "$TMPFILE"
echo "#ifndef TSPDF_ASSETS_H" >> "$TMPFILE"
echo "#define TSPDF_ASSETS_H" >> "$TMPFILE"
echo "#include <stddef.h>" >> "$TMPFILE"

embed_file() {
    local path="$1"
    local varname="$2"
    echo "" >> "$TMPFILE"
    echo "static const unsigned char ${varname}[] = {" >> "$TMPFILE"
    od -An -tx1 -v < "$path" | sed 's/[0-9a-f]\{2\}/0x&,/g' >> "$TMPFILE"
    echo "};" >> "$TMPFILE"
    echo "static const size_t ${varname}_len = sizeof(${varname});" >> "$TMPFILE"
}

# Core assets
embed_file "web/templates/base.html" "asset_base_html"
embed_file "web/templates/index.html" "asset_index_html"
embed_file "web/static/style.css" "asset_style_css"
embed_file "web/static/app.js" "asset_app_js"

# Tool templates
mapfile -t TOOL_TEMPLATES < <(find web/templates/tools -maxdepth 1 -type f -name '*.html' | LC_ALL=C sort)
for f in "${TOOL_TEMPLATES[@]}"; do
    # Convert filename to variable name: web/templates/tools/merge.html -> asset_tool_merge
    base=$(basename "$f" .html)
    varname="asset_tool_$(echo "$base" | tr '-' '_')"
    embed_file "$f" "$varname"
done

echo "" >> "$TMPFILE"
echo "#endif" >> "$TMPFILE"

mv "$TMPFILE" "$OUT"
echo "Generated $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes)"
