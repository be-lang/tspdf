#!/usr/bin/env bash
# Generates the single-file amalgamation: <out>/tspdf.h + <out>/tspdf.c
# (default out: build/amalgamation). bash + awk/sed only, no other tools.
#
# tspdf.h = the public headers reachable from include/tspdf.h, concatenated in
# dependency order. tspdf.c = the internal headers plus every library .c file,
# in the Makefile's source order. Project-internal #include "..." lines are
# stripped (each header is inlined exactly once, include guards kept), and
# unconditional #include <...> lines are hoisted, deduplicated, to the top of
# each output file. Conditional system includes (e.g. <sys/random.h> under
# #ifdef __linux__) stay in place so platform guards keep working.
#
# Because everything becomes one translation unit, file-local `static` names
# that repeat across .c files (and macros redefined across .c files, e.g. the
# SHA-256/512 round helpers) would collide. Both are detected automatically:
# colliding statics are renamed via a #define/#undef pair scoped to their
# file's region, and every macro a .c file defines is #undef'd at the end of
# its region. `make amalgamation` proves the result compiles under -Werror.
set -eu

cd "$(dirname "$0")/.."
OUT_DIR="${1:-build/amalgamation}"

VMAJ=$(sed -n 's/^#define TSPDF_VERSION_MAJOR *//p' include/tspdf/version.h)
VMIN=$(sed -n 's/^#define TSPDF_VERSION_MINOR *//p' include/tspdf/version.h)
VPAT=$(sed -n 's/^#define TSPDF_VERSION_PATCH *//p' include/tspdf/version.h)
VERSION="$VMAJ.$VMIN.$VPAT"

# Parse sources.mk to derive the file lists — single source of truth.
# awk extracts each multi-line Make variable (VAR = \ ... lines) as a
# whitespace-separated list of values (stripping backslash continuations,
# $(SRCDIR) substitution, and blank/comment lines).
parse_mk_var() {
    local var="$1"
    awk -v v="$var" '
        BEGIN { in_var = 0 }
        # Start capturing when we see "VAR = ..." or "VAR = \"
        $0 ~ "^" v "[ \t]*=" {
            # strip "VAR = " prefix and any trailing backslash
            sub("^" v "[ \t]*=[ \t]*", "")
            sub(/[ \t]*\\[ \t]*$/, "")
            gsub(/\$\(SRCDIR\)/, "src")
            if ($0 != "") print $0
            in_var = 1
            next
        }
        # Continue capturing continuation lines (end with \)
        in_var && /\\[ \t]*$/ {
            sub(/[ \t]*\\[ \t]*$/, "")
            gsub(/\$\(SRCDIR\)/, "src")
            sub(/^[ \t]+/, "")
            if ($0 != "") print $0
            next
        }
        # Guard: blank/whitespace-only lines while in_var are an error
        in_var && /^[ \t]*$/ {
            print "error: blank line inside variable block in sources.mk" > "/dev/stderr"
            exit 1
        }
        # Last continuation line (no trailing \)
        in_var {
            sub(/^[ \t]+/, "")
            gsub(/\$\(SRCDIR\)/, "src")
            if ($0 != "" && $0 !~ /^#/) print $0
            in_var = 0
        }
    ' sources.mk
}

# Derive the lists from sources.mk (single source of truth).
PUBLIC_HEADERS=$(parse_mk_var PUBLIC_HEADERS)
INTERNAL_HEADERS=$(parse_mk_var INTERNAL_HEADERS)
# ALL_SOURCES in the amalgamation = LIB_SOURCES + OPS_SOURCES + TSPR_SOURCES + CRYPTO_SOURCES
# ops.c comes after TSPR_SOURCES because it calls reader functions at runtime
# (it only needs the declarations at compile time via tspdf.h, but dependency
# ordering is clearest this way and avoids forward-reference surprises).
SOURCES="$(parse_mk_var LIB_SOURCES)
$(parse_mk_var TSPR_SOURCES)
$(parse_mk_var OPS_SOURCES)
$(parse_mk_var CRYPTO_SOURCES)"

# A new file under src/ or include/ that this script does not know about must
# fail loudly, not ship a silently incomplete amalgamation. The umbrella
# headers are known-but-not-amalgamated: they only #include the closures
# listed above (this file replaces them), so they are exempt from the scan
# without being emitted.
UMBRELLA_HEADERS=$(parse_mk_var UMBRELLA_HEADERS)
KNOWN="$PUBLIC_HEADERS $INTERNAL_HEADERS $SOURCES"
missing=""
for f in $(find src include -name '*.c' -o -name '*.h' | sort); do
    case " $(echo $KNOWN $UMBRELLA_HEADERS) " in
        *" $f "*) ;;
        *) missing="$missing $f" ;;
    esac
done
if [ -n "$missing" ]; then
    echo "amalgamate.sh: unknown src/include file(s):$missing" >&2
    echo "add them to the file lists in sources.mk" >&2
    exit 1
fi
for f in $KNOWN; do
    [ -f "$f" ] || { echo "amalgamate.sh: missing file: $f" >&2; exit 1; }
done

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Preprocessor-conditional depth at which an include is "unconditional":
# depth 0 in a .c file, depth 1 (inside only the include guard) in a header.
hoist_depth() {
    case "$1" in *.h) echo 1 ;; *) echo 0 ;; esac
}

# Print the system includes of a file that are safe to hoist, normalized.
collect_system_includes() {
    awk -v lim="$(hoist_depth "$1")" '
        /^[ \t]*#[ \t]*include[ \t]*</ {
            if (depth == lim && match($0, /<[^>]+>/))
                print "#include " substr($0, RSTART, RLENGTH)
        }
        /^[ \t]*#[ \t]*(if|ifdef|ifndef)/ { depth++ }
        /^[ \t]*#[ \t]*endif/             { depth-- }
    ' "$1"
}

# Emit a file body: strip project includes, drop hoisted system includes.
emit_body() {
    awk -v lim="$(hoist_depth "$1")" '
        /^[ \t]*#[ \t]*include[ \t]*"/ {
            sub(/^[ \t]*#[ \t]*include[ \t]*/, "")
            print "/* amalgamated: #include " $0 " */"
            next
        }
        /^[ \t]*#[ \t]*include[ \t]*</ {
            if (depth == lim) next
        }
        { print }
        /^[ \t]*#[ \t]*(if|ifdef|ifndef)/ { depth++ }
        /^[ \t]*#[ \t]*endif/             { depth-- }
    ' "$1"
}

# Print the top-level static names (functions and objects) defined in a file.
static_names() {
    awk '
        /^static[ \t]/ {
            rest = $0
            while (match(rest, /[A-Za-z_][A-Za-z0-9_]*/)) {
                name  = substr(rest, RSTART, RLENGTH)
                after = substr(rest, RSTART + RLENGTH)
                sub(/^[ \t]*/, "", after)
                c = substr(after, 1, 1)
                if (c == "(" || c == "[" || c == "=" || c == ";") {
                    print name
                    break
                }
                rest = substr(rest, RSTART + RLENGTH)
            }
        }
    ' "$1" | sort -u
}

# Print feature-test macro defines (#define _..._SOURCE ...). In a single TU
# they must precede every system include, so they are hoisted to the very top
# of tspdf.c. The in-place copies stay: identical redefinition is legal C11.
feature_defines() {
    awk '/^[ \t]*#[ \t]*define[ \t]+_[A-Za-z0-9_]*SOURCE([ \t]|$)/' "$1"
}

# Print the macro names #define'd at any level in a file.
macro_names() {
    awk '
        /^[ \t]*#[ \t]*define[ \t]/ {
            line = $0
            sub(/^[ \t]*#[ \t]*define[ \t]+/, "", line)
            if (match(line, /^[A-Za-z_][A-Za-z0-9_]*/))
                print substr(line, RSTART, RLENGTH)
        }
    ' "$1" | sort -u
}

# Static-name collision map: "file name" pairs over headers and sources.
: > "$TMP/statics"
for f in $KNOWN; do
    static_names "$f" | sed "s|^|$f |" >> "$TMP/statics"
done
# Names defined static in more than one file collide in a single TU.
awk '{ print $2 }' "$TMP/statics" | sort | uniq -d > "$TMP/collisions"

# A collision between two *headers* cannot be fixed by region-scoped renames
# (headers are public text); it needs a source-level fix. None exist today.
awk '$1 ~ /\.h$/ { print $2 }' "$TMP/statics" | sort | uniq -d > "$TMP/hdr_dups"
if [ -s "$TMP/hdr_dups" ]; then
    echo "amalgamate.sh: static name defined in two headers:" >&2
    cat "$TMP/hdr_dups" >&2
    exit 1
fi

banner() {
    cat <<EOF
/*
 * tspdf $VERSION — single-file amalgamation ($1).
 * PDF toolkit in pure C11, zero dependencies. MIT license (see LICENSE).
 * Generated by scripts/amalgamate.sh from https://github.com/be-lang/tspdf
 * — do not edit; edit the original sources and regenerate.
 *
 * Usage: compile tspdf.c as C11 (needs -lm), #include "tspdf.h" in your code.
 */
EOF
}

mkdir -p "$OUT_DIR"

# ---- tspdf.h ----
{
    banner "tspdf.h"
    echo "#ifndef TSPDF_AMALGAMATION_H"
    echo "#define TSPDF_AMALGAMATION_H"
    echo
    for f in $PUBLIC_HEADERS; do collect_system_includes "$f"; done | awk '!seen[$0]++'
    for f in $PUBLIC_HEADERS; do
        echo
        echo "/* ==== $f ==== */"
        emit_body "$f"
    done
    echo
    echo "#endif /* TSPDF_AMALGAMATION_H */"
} > "$OUT_DIR/tspdf.h"

# ---- tspdf.c ----
{
    banner "tspdf.c"
    for f in $SOURCES; do feature_defines "$f"; done | awk '!seen[$0]++'
    echo "#include \"tspdf.h\""
    echo
    { for f in $INTERNAL_HEADERS $SOURCES; do collect_system_includes "$f"; done; } \
        | awk '!seen[$0]++'
    for f in $INTERNAL_HEADERS; do
        echo
        echo "/* ==== $f ==== */"
        emit_body "$f"
    done
    for f in $SOURCES; do
        stem=$(basename "$f" .c)
        echo
        echo "/* ==== $f ==== */"
        # Rename this file's colliding statics for the span of its region.
        renames=$(static_names "$f" | sort - "$TMP/collisions" | uniq -d)
        for n in $renames; do
            echo "#define $n tspdf_amal_${stem}_$n"
        done
        emit_body "$f"
        for n in $renames; do
            echo "#undef $n"
        done
        # File-private macros must not leak into the next file's region.
        for n in $(macro_names "$f"); do
            echo "#undef $n"
        done
    done
} > "$OUT_DIR/tspdf.c"

echo "amalgamation: $OUT_DIR/tspdf.h $OUT_DIR/tspdf.c (version $VERSION)"
