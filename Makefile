CC ?= gcc
# CFLAGS/CPPFLAGS/LDFLAGS are overridable so distro packagers (dpkg-buildflags,
# makepkg, abuild, nix) can inject their flags. The required flags live in
# REQUIRED_CFLAGS so a user CFLAGS override can never drop the C11/warning
# baseline; internal rules use ALL_CFLAGS.
CFLAGS ?= -g -O2
CPPFLAGS ?=
LDFLAGS ?=
REQUIRED_CFLAGS = -Wall -Wextra -std=c11
ALL_CFLAGS = $(CFLAGS) $(REQUIRED_CFLAGS)
SRCDIR = src
BUILDDIR = build
PREFIX ?= /usr/local
DESTDIR ?=

# Single source of truth for the version: parse include/tspdf/version.h so the
# Makefile, the compiled-in TSPDF_VERSION_STRING, and the pkg-config file can
# never drift apart. (No shell-4-isms; plain sed works on macOS bash 3.2 too.)
VERSION_MAJOR := $(shell sed -n 's/^\#define TSPDF_VERSION_MAJOR *//p' include/tspdf/version.h)
VERSION_MINOR := $(shell sed -n 's/^\#define TSPDF_VERSION_MINOR *//p' include/tspdf/version.h)
VERSION_PATCH := $(shell sed -n 's/^\#define TSPDF_VERSION_PATCH *//p' include/tspdf/version.h)
VERSION := $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)

# Shared-library naming and ABI policy. Pre-1.0 a minor release may break the
# ABI, so the SONAME carries MAJOR.MINOR (libtspdf.so.0.2 for any 0.2.x) and
# bumps on every minor — a binary linked against 0.1 never silently loads an
# incompatible 0.2. From 1.0 on the SONAME is just MAJOR and bumps only on an
# actual ABI break. Symlink chain: libtspdf.so -> $(SHLIB_SONAME) -> real file.
ifeq ($(VERSION_MAJOR),0)
SHLIB_ABI  = $(VERSION_MAJOR).$(VERSION_MINOR)
SHLIB_TAIL = $(VERSION_PATCH)
else
SHLIB_ABI  = $(VERSION_MAJOR)
SHLIB_TAIL = $(VERSION_MINOR).$(VERSION_PATCH)
endif

# Only tspdf_* / tspr_* are public API; the export lists below keep every
# internal helper (aes_*, sha256_*, deflate_*, jpeg_*, png_*, qr_*, ttf_*, ...)
# out of the dynamic symbol table so a system-wide shared library cannot
# interpose symbols of other code at runtime.
SHLIB_EXPORTS_MAP = scripts/libtspdf.map
SHLIB_EXPORTS_EXP = scripts/libtspdf.exp

# ELF and Mach-O spell versioning and export filtering differently, hence the
# uname guard. The Darwin branch follows platform conventions but is untested
# locally (no macOS machine here); the CI macOS job runs a shared-lib smoke
# build so a broken dylib link fails there, not at a user's desk.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SHLIB_FILE   = libtspdf.$(SHLIB_ABI).$(SHLIB_TAIL).dylib
SHLIB_SONAME = libtspdf.$(SHLIB_ABI).dylib
SHLIB_LINK   = libtspdf.dylib
SHLIB_LDFLAGS = -dynamiclib -install_name @rpath/$(SHLIB_SONAME) \
	-compatibility_version $(SHLIB_ABI) -current_version $(VERSION) \
	-exported_symbols_list $(SHLIB_EXPORTS_EXP)
SHLIB_EXPORTS = $(SHLIB_EXPORTS_EXP)
else
SHLIB_FILE   = libtspdf.so.$(SHLIB_ABI).$(SHLIB_TAIL)
SHLIB_SONAME = libtspdf.so.$(SHLIB_ABI)
SHLIB_LINK   = libtspdf.so
SHLIB_LDFLAGS = -shared -Wl,-soname,$(SHLIB_SONAME) \
	-Wl,--version-script=$(SHLIB_EXPORTS_MAP)
SHLIB_EXPORTS = $(SHLIB_EXPORTS_MAP)
endif

# Sanitizer build flags for `make test-asan`. Compiler-builtin instrumentation
# only (AddressSanitizer + UndefinedBehaviorSanitizer + LeakSanitizer), so the
# zero-dependency promise holds. -O1 keeps frames/inlining sane for readable
# reports; -fno-sanitize-recover=all makes any UB abort instead of merely
# logging, so a CI run actually fails on the first violation.
SAN_CFLAGS = -Wall -Wextra -std=c11 -g -O1 \
	-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

# libFuzzer build for `make fuzz`. Requires clang (the fuzzing runtime ships with
# the compiler, so the zero-dependency promise holds — no vendored library). The
# harnesses live in fuzz/ and feed raw bytes into the untrusted-input parsers.
# Override on the command line for AFL etc., e.g. `make fuzz FUZZ_CC=afl-clang-fast`.
FUZZ_CC ?= clang
FUZZ_CFLAGS = -Wall -Wextra -std=c11 -g -O1 \
	-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
FUZZ_BIN = $(BUILDDIR)/fuzz

LIB_SOURCES = \
	$(SRCDIR)/util/buffer.c \
	$(SRCDIR)/util/arena.c \
	$(SRCDIR)/util/pdftext.c \
	$(SRCDIR)/pdf/pdf_writer.c \
	$(SRCDIR)/pdf/pdf_stream.c \
	$(SRCDIR)/pdf/pdf_base14.c \
	$(SRCDIR)/pdf/tspdf_writer.c \
	$(SRCDIR)/font/ttf_parser.c \
	$(SRCDIR)/font/font_subset.c \
	$(SRCDIR)/layout/layout.c \
	$(SRCDIR)/image/jpeg_embed.c \
	$(SRCDIR)/image/png_decoder.c \
	$(SRCDIR)/compress/deflate.c \
	$(SRCDIR)/qr/qr_encode.c \
	$(SRCDIR)/tspdf_error.c

TSPR_SOURCES = \
	$(SRCDIR)/reader/tspr_parser.c \
	$(SRCDIR)/reader/tspr_xref.c \
	$(SRCDIR)/reader/tspr_pages.c \
	$(SRCDIR)/reader/tspr_serialize.c \
	$(SRCDIR)/reader/tspr_crypt.c \
	$(SRCDIR)/reader/tspr_document.c \
	$(SRCDIR)/reader/tspr_doctree.c \
	$(SRCDIR)/reader/tspr_attach.c \
	$(SRCDIR)/reader/tspr_metadata.c \
	$(SRCDIR)/reader/tspr_content.c \
	$(SRCDIR)/reader/tspr_resources.c \
	$(SRCDIR)/reader/tspr_annot.c \
	$(SRCDIR)/reader/tspr_text.c

CRYPTO_SOURCES = \
	$(SRCDIR)/crypto/md5.c \
	$(SRCDIR)/crypto/sha256.c \
	$(SRCDIR)/crypto/sha512.c \
	$(SRCDIR)/crypto/rc4.c \
	$(SRCDIR)/crypto/aes.c

ALL_SOURCES = $(LIB_SOURCES) $(TSPR_SOURCES) $(CRYPTO_SOURCES)

CLI_SOURCES = \
	cli/main.c \
	cli/commands.c \
	cli/cmd_merge.c \
	cli/cmd_split.c \
	cli/cmd_rotate.c \
	cli/cmd_delete.c \
	cli/cmd_reorder.c \
	cli/cmd_encrypt.c \
	cli/cmd_decrypt.c \
	cli/cmd_metadata.c \
	cli/cmd_info.c \
	cli/cmd_watermark.c \
	cli/cmd_compress.c \
	cli/cmd_img2pdf.c \
	cli/cmd_qrcode.c \
	cli/cmd_md2pdf.c \
	cli/cmd_text.c \
	cli/cmd_pagenum.c \
	cli/cmd_attach.c \
	cli/server.c

CLI_TARGET = $(BUILDDIR)/tspdf
WEB_EMBED_CORE = \
	web/templates/base.html \
	web/templates/index.html \
	web/static/style.css \
	web/static/app.js
WEB_EMBED_TOOLS = $(sort $(wildcard web/templates/tools/*.html))
WEB_EMBED_INPUTS = scripts/embed_assets.sh $(WEB_EMBED_CORE) $(WEB_EMBED_TOOLS)

.PHONY: all cli install install-lib uninstall clean test test-all test-cli test-reader test-crypto \
        test-asan test-asan-bin test-asan-reader test-asan-reader-bin test-external \
        check ci fuzz fuzz-corpus print-version amalgamation \
        lib shared demo bench bench-reader bench-crypto minimal reader-demo generate-test-pdfs

# --- Default: build the CLI ---

all: $(CLI_TARGET)

# Phony alias: a directory named `cli/` exists, so `make cli` must not be a no-op.
cli: $(CLI_TARGET)

cli/assets.h: $(WEB_EMBED_INPUTS)
	bash scripts/embed_assets.sh

$(CLI_TARGET): cli/assets.h $(CLI_SOURCES) $(ALL_SOURCES)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(CLI_SOURCES) $(ALL_SOURCES) -lm

# --- Install / uninstall ---
#
# Two install faces:
#   * `install`      — CLI binary + the full library install (headers + libs + .pc).
#   * `install-lib`  — just the library, headers and pkg-config (no CLI).
#
# All paths honour DESTDIR (staged installs) and PREFIX (default /usr/local).
#
# Relocatability: the public headers keep their in-tree relative includes
# (e.g. pdf/pdf_stream.h does `#include "../util/buffer.h"`), so we install them
# under $(PREFIX)/include/tspdf/ preserving the src/ subdirectory layout. A tiny
# marker header (tspdf_installed.h) is dropped next to tspdf.h so the umbrella
# switches to the <tspdf/...> include form. Consumers then use:
#     #include <tspdf/tspdf.h>    -I$(PREFIX)/include   -ltspdf -lm
# with no reference back to the (possibly deleted/moved) source tree.

DESTBIN     = $(DESTDIR)$(PREFIX)/bin
DESTLIB     = $(DESTDIR)$(PREFIX)/lib
DESTINC     = $(DESTDIR)$(PREFIX)/include/tspdf
DESTPKGCFG  = $(DESTDIR)$(PREFIX)/lib/pkgconfig
DESTMAN1    = $(DESTDIR)$(PREFIX)/share/man/man1

# Public headers to install, listed WITH the subdirectory they live in so the
# relative "../foo/bar.h" includes still resolve under $(PREFIX)/include/tspdf/.
INSTALL_HEADERS_ROOT = \
	$(SRCDIR)/tspdf_error.h
INSTALL_HEADERS_PDF = \
	$(SRCDIR)/pdf/tspdf_writer.h \
	$(SRCDIR)/pdf/pdf_writer.h \
	$(SRCDIR)/pdf/pdf_stream.h \
	$(SRCDIR)/pdf/pdf_base14.h
INSTALL_HEADERS_FONT = \
	$(SRCDIR)/font/ttf_parser.h \
	$(SRCDIR)/font/font_subset.h
INSTALL_HEADERS_IMAGE = \
	$(SRCDIR)/image/jpeg_embed.h
INSTALL_HEADERS_UTIL = \
	$(SRCDIR)/util/buffer.h \
	$(SRCDIR)/util/arena.h
INSTALL_HEADERS_READER = \
	$(SRCDIR)/reader/tspr.h \
	$(SRCDIR)/reader/tspr_overlay.h
INSTALL_HEADERS_LAYOUT = \
	$(SRCDIR)/layout/layout.h

install: install-lib $(CLI_TARGET)
	install -d $(DESTBIN)
	install -m 755 $(CLI_TARGET) $(DESTBIN)/tspdf
	install -d $(DESTMAN1)
	install -m 644 docs/tspdf.1 $(DESTMAN1)/tspdf.1
	@case ":$$PATH:" in \
	  *":$(PREFIX)/bin:"*) ;; \
	  *) echo "note: $(PREFIX)/bin is not on your PATH; add it to run tspdf" ;; \
	esac

# Install the static + shared libraries, public headers, and pkg-config file.
install-lib: $(BUILDDIR)/libtspdf.a shared tspdf.pc.in include/tspdf/version.h
	install -d $(DESTLIB)
	install -m 644 $(BUILDDIR)/libtspdf.a $(DESTLIB)/libtspdf.a
	install -m 755 $(BUILDDIR)/$(SHLIB_FILE) $(DESTLIB)/$(SHLIB_FILE)
	ln -sf $(SHLIB_FILE) $(DESTLIB)/$(SHLIB_SONAME)
	ln -sf $(SHLIB_SONAME) $(DESTLIB)/$(SHLIB_LINK)
	install -d $(DESTINC) $(DESTINC)/pdf $(DESTINC)/font \
	          $(DESTINC)/image $(DESTINC)/util $(DESTINC)/reader $(DESTINC)/layout
	install -m 644 include/tspdf.h $(DESTINC)/tspdf.h
	install -m 644 include/tspdf_overlay.h $(DESTINC)/tspdf_overlay.h
	install -m 644 include/tspdf/version.h $(DESTINC)/version.h
	install -m 644 $(INSTALL_HEADERS_ROOT)   $(DESTINC)/
	install -m 644 $(INSTALL_HEADERS_PDF)    $(DESTINC)/pdf/
	install -m 644 $(INSTALL_HEADERS_FONT)   $(DESTINC)/font/
	install -m 644 $(INSTALL_HEADERS_IMAGE)  $(DESTINC)/image/
	install -m 644 $(INSTALL_HEADERS_UTIL)   $(DESTINC)/util/
	install -m 644 $(INSTALL_HEADERS_READER) $(DESTINC)/reader/
	install -m 644 $(INSTALL_HEADERS_LAYOUT) $(DESTINC)/layout/
	printf '/* installed-layout marker: makes <tspdf/tspdf.h> use the <tspdf/...> include form */\n#ifndef TSPDF_INSTALLED_H\n#define TSPDF_INSTALLED_H\n#endif\n' > $(DESTINC)/tspdf_installed.h
	install -d $(DESTPKGCFG)
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(VERSION)|g' \
	    tspdf.pc.in > $(DESTPKGCFG)/tspdf.pc

uninstall:
	rm -f $(DESTBIN)/tspdf
	rm -f $(DESTMAN1)/tspdf.1
	rm -f $(DESTLIB)/libtspdf.a
	rm -f $(DESTLIB)/$(SHLIB_FILE) $(DESTLIB)/$(SHLIB_SONAME) $(DESTLIB)/$(SHLIB_LINK)
	rm -f $(DESTPKGCFG)/tspdf.pc
	rm -rf $(DESTINC)

# Print the version parsed from version.h (used by tooling / CI).
print-version:
	@echo $(VERSION)

clean:
	rm -rf $(BUILDDIR) output.pdf

# --- Tests ---

test: $(BUILDDIR)/test_runner
	./$(BUILDDIR)/test_runner

test-reader: $(BUILDDIR)/test_reader
	./$(BUILDDIR)/test_reader

test-crypto: $(BUILDDIR)/test_crypto
	./$(BUILDDIR)/test_crypto

test-cli: $(CLI_TARGET)
	bash tests/test_cli.sh

test-all: test test-reader test-crypto test-cli

# --- External PDF conformance gate (qpdf --check / mutool) ---
#
# For a write-from-scratch PDF library the core correctness claim is that output
# is spec-valid and openable by *real* readers, not merely self-round-trippable.
# `test-external` drives every CLI command plus reader round-trips and validates
# the output with whatever conformance checkers are installed (qpdf, mutool).
#
# Test-time only: the verification binaries are NOT linked into tspdf, so the
# zero-dependency promise holds. The script skips gracefully (exit 0) when no
# checker is present — mirroring the curl/python3 guards in test_cli.sh — so it
# is safe to call on an offline box. It is intentionally NOT part of `test-all`
# or `check` (which must stay deterministic and offline-green); CI installs
# qpdf + mupdf-tools and runs this target as a dedicated job so the gate is real.
test-external: $(CLI_TARGET)
	bash tests/test_external.sh

# --- Sanitizer test targets (ASan + UBSan + LeakSanitizer) ---
#
# The codebase is full of hand-written pointer/length arithmetic over untrusted
# input, so a standing instrumented run is the cheapest safety net. Each target
# does a clean rebuild with $(SAN_CFLAGS) and runs the test binaries with leak
# detection and abort-on-error enabled. It recurses so the binaries are rebuilt
# with the sanitizer CFLAGS even if a plain `make` already cached -O2 objects.
#
# ASAN_OPTIONS=detect_leaks=1 turns on LeakSanitizer; abort_on_error makes ASan
# faults non-recoverable, and UBSAN_OPTIONS=halt_on_error pairs with the
# -fno-sanitize-recover=all compile flag so any UB aborts the run (CI-friendly).
#
# `test-asan` covers the writer/layout/font/image/compress and crypto-primitive
# code (test_runner + test_crypto). `make test-cli` rebuilds the CLI with
# default CFLAGS, so it is intentionally not part of this target.
#
# Reader/parser coverage lives in the separate `test-asan-reader` target so the
# always-green `test-asan` does not depend on parser memory-management fixes
# that are still in flight.
test-asan:
	$(MAKE) clean
	$(MAKE) test-asan-bin CFLAGS="$(SAN_CFLAGS)"

test-asan-bin: $(BUILDDIR)/test_runner $(BUILDDIR)/test_crypto
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./$(BUILDDIR)/test_runner
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./$(BUILDDIR)/test_crypto

# Run the reader/parser test suite under the sanitizers. Kept separate from
# `test-asan` because it exercises the largest body of untrusted-input parsing.
test-asan-reader:
	$(MAKE) clean
	$(MAKE) test-asan-reader-bin CFLAGS="$(SAN_CFLAGS)"

test-asan-reader-bin: $(BUILDDIR)/test_reader
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./$(BUILDDIR)/test_reader

# --- Fuzzing (libFuzzer / AFL) ---
#
# `make fuzz` builds one binary per untrusted-input parser in $(FUZZ_BIN)/. Each
# is a libFuzzer target (clang -fsanitize=fuzzer,address,undefined) over the
# from-scratch parsers that consume attacker-controlled bytes: the PDF reader,
# the DEFLATE/zlib inflater, the TrueType parser, and the PNG decoder — plus a
# compressor round-trip target (fuzz_deflate) that asserts compress→inflate is
# byte-identical. A seed corpus is dropped alongside (see fuzz-corpus). Run e.g.:
#
#   make fuzz
#   ./build/fuzz/fuzz_reader fuzz/corpus/reader
#   ./build/fuzz/fuzz_png    fuzz/corpus/png -max_total_time=60
#
# Compiler-only — the fuzzing runtime ships with clang, so zero vendored deps.
FUZZ_TARGETS = $(FUZZ_BIN)/fuzz_reader $(FUZZ_BIN)/fuzz_inflate \
               $(FUZZ_BIN)/fuzz_deflate $(FUZZ_BIN)/fuzz_ttf $(FUZZ_BIN)/fuzz_png

fuzz: $(FUZZ_TARGETS) fuzz-corpus

$(FUZZ_BIN)/fuzz_reader: fuzz/fuzz_reader.c $(ALL_SOURCES)
	@mkdir -p $(FUZZ_BIN)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_SOURCES) $(TSPR_SOURCES) $(CRYPTO_SOURCES) -lm

$(FUZZ_BIN)/fuzz_inflate: fuzz/fuzz_inflate.c $(SRCDIR)/compress/deflate.c $(SRCDIR)/util/buffer.c
	@mkdir -p $(FUZZ_BIN)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) $(LDFLAGS) -o $@ $^ -lm

$(FUZZ_BIN)/fuzz_deflate: fuzz/fuzz_deflate.c $(SRCDIR)/compress/deflate.c $(SRCDIR)/util/buffer.c
	@mkdir -p $(FUZZ_BIN)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) $(LDFLAGS) -o $@ $^ -lm

$(FUZZ_BIN)/fuzz_ttf: fuzz/fuzz_ttf.c $(SRCDIR)/font/ttf_parser.c
	@mkdir -p $(FUZZ_BIN)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) $(LDFLAGS) -o $@ $^ -lm

$(FUZZ_BIN)/fuzz_png: fuzz/fuzz_png.c $(SRCDIR)/image/png_decoder.c \
                      $(SRCDIR)/compress/deflate.c $(SRCDIR)/util/buffer.c
	@mkdir -p $(FUZZ_BIN)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_CFLAGS) $(LDFLAGS) -o $@ $^ -lm

# Seed the reader corpus with the synthetic test PDFs so libFuzzer starts from
# real, structurally-valid inputs instead of random noise. We copy the existing
# committed fixtures rather than regenerating them, so `make fuzz` never mutates
# tracked files; if they are absent (e.g. a fresh checkout that cleaned them),
# build them first via `make generate-test-pdfs`.
fuzz-corpus:
	@mkdir -p fuzz/corpus/reader
	@if [ -f tests/data/one_page.pdf ]; then \
		cp -f tests/data/one_page.pdf fuzz/corpus/reader/; fi
	@if [ -f tests/data/three_pages.pdf ]; then \
		cp -f tests/data/three_pages.pdf fuzz/corpus/reader/; fi
	@echo "Seed corpus ready under fuzz/corpus/ (run 'make generate-test-pdfs' if reader seeds are missing)"

# --- CI gate ---
#
# `check` is the warnings-as-errors build+test gate used by CI. It does a clean
# rebuild with -Werror layered on the default CFLAGS so the zero-warning
# invariant (-Wall -Wextra clean) can never silently regress, then runs the
# full suite (unit + reader + crypto + CLI). `make lib` is included so the
# embedded-asset/library build path is exercised too. Compiler/make only — no
# third-party tooling, matching the zero-dependency promise.
check:
	$(MAKE) clean
	$(MAKE) all      CFLAGS="$(CFLAGS) -Werror"
	$(MAKE) lib      CFLAGS="$(CFLAGS) -Werror"
	$(MAKE) test-all CFLAGS="$(CFLAGS) -Werror"

# Alias so CI configs can call a single conventional target.
ci: check

$(BUILDDIR)/test_runner: tests/test_main.c $(LIB_SOURCES)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_SOURCES) -lm

$(BUILDDIR)/test_reader: tests/test_reader.c $(ALL_SOURCES)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $< $(TSPR_SOURCES) $(LIB_SOURCES) $(CRYPTO_SOURCES) -lm

$(BUILDDIR)/test_crypto: tests/test_crypto.c $(CRYPTO_SOURCES)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $< $(CRYPTO_SOURCES) -lm

# --- Library (for embedding in other projects) ---

lib: $(BUILDDIR)/libtspdf.a

$(BUILDDIR)/libtspdf.a: $(ALL_SOURCES)
	@mkdir -p $(BUILDDIR)/obj
	for f in $(ALL_SOURCES); do $(CC) $(CPPFLAGS) $(ALL_CFLAGS) -c $$f -o $(BUILDDIR)/obj/$$(basename $$f .c).o; done
	ar rcs $@ $(BUILDDIR)/obj/*.o

# Shared library, properly versioned (see SHLIB_* above): the real file is
# $(SHLIB_FILE) with SONAME $(SHLIB_SONAME), plus the SONAME and libtspdf.so
# symlinks (dylib naming on Darwin). The export list restricts the dynamic
# symbol table to the public tspdf_*/tspr_* API; tests/test_cli.sh asserts
# both the SONAME and the symbol filter.
shared: $(BUILDDIR)/$(SHLIB_FILE)
	ln -sf $(SHLIB_FILE) $(BUILDDIR)/$(SHLIB_SONAME)
	ln -sf $(SHLIB_SONAME) $(BUILDDIR)/$(SHLIB_LINK)

$(BUILDDIR)/$(SHLIB_FILE): $(ALL_SOURCES) $(SHLIB_EXPORTS)
	@mkdir -p $(BUILDDIR)/shobj
	for f in $(ALL_SOURCES); do $(CC) $(CPPFLAGS) $(ALL_CFLAGS) -fPIC -c $$f -o $(BUILDDIR)/shobj/$$(basename $$f .c).o; done
	$(CC) $(LDFLAGS) $(SHLIB_LDFLAGS) -o $@ $(BUILDDIR)/shobj/*.o -lm

# --- Single-file amalgamation ---
#
# `make amalgamation` generates build/amalgamation/tspdf.{c,h} via
# scripts/amalgamate.sh and then proves the result: a standalone hardened
# -Werror compile, a link + run of examples/minimal.c against it, and a
# qpdf --check of the produced PDF when qpdf is installed (test-time only,
# skipped gracefully — the zero-dependency promise holds).
AMAL_DIR = $(BUILDDIR)/amalgamation
AMAL_CFLAGS = -Wall -Wextra -std=c11 -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -Werror

amalgamation:
	bash scripts/amalgamate.sh $(AMAL_DIR)
	$(CC) $(AMAL_CFLAGS) -c $(AMAL_DIR)/tspdf.c -o $(AMAL_DIR)/tspdf.o
	sed 's|#include "../include/tspdf.h"|#include "tspdf.h"|' examples/minimal.c \
	    > $(AMAL_DIR)/minimal_amal.c
	$(CC) $(AMAL_CFLAGS) -I $(AMAL_DIR) -o $(AMAL_DIR)/minimal_amal \
	    $(AMAL_DIR)/minimal_amal.c $(AMAL_DIR)/tspdf.o -lm
	cd $(AMAL_DIR) && ./minimal_amal
	@if command -v qpdf > /dev/null 2>&1; then \
		qpdf --check $(AMAL_DIR)/minimal.pdf && echo "qpdf --check: OK"; \
	else \
		echo "note: qpdf not installed; skipping conformance check"; \
	fi

# --- Examples & benchmarks ---

demo: $(BUILDDIR)/tspdf_demo
	./$(BUILDDIR)/tspdf_demo

$(BUILDDIR)/tspdf_demo: examples/demo.c $(LIB_SOURCES)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_SOURCES) -lm

minimal: $(BUILDDIR)/minimal
	./$(BUILDDIR)/minimal

$(BUILDDIR)/minimal: examples/minimal.c $(LIB_SOURCES)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_SOURCES) -lm

reader-demo: $(BUILDDIR)/reader_demo
	./$(BUILDDIR)/reader_demo

$(BUILDDIR)/reader_demo: examples/reader_demo.c $(ALL_SOURCES)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_SOURCES) $(TSPR_SOURCES) $(CRYPTO_SOURCES) -lm

bench: $(BUILDDIR)/bench
	./$(BUILDDIR)/bench

$(BUILDDIR)/bench: examples/benchmark.c $(LIB_SOURCES)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_SOURCES) -lm

bench-reader: $(BUILDDIR)/bench_reader
	./$(BUILDDIR)/bench_reader

$(BUILDDIR)/bench_reader: examples/bench_reader.c $(ALL_SOURCES)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_SOURCES) $(TSPR_SOURCES) $(CRYPTO_SOURCES) -lm

bench-crypto: $(BUILDDIR)/bench_crypto
	./$(BUILDDIR)/bench_crypto

$(BUILDDIR)/bench_crypto: tests/bench_crypto.c $(CRYPTO_SOURCES)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $< $(CRYPTO_SOURCES) -lm

generate-test-pdfs: $(BUILDDIR)/generate_test_pdfs
	@mkdir -p tests/data
	./$(BUILDDIR)/generate_test_pdfs

$(BUILDDIR)/generate_test_pdfs: tests/generate_test_pdfs.c $(LIB_SOURCES)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_SOURCES) -lm

# --- WebAssembly build (optional; requires emcc) ---
#
# `make wasm` compiles the whole library plus wasm/shim.c into an ES6 module
# (wasm/dist/tspdf.js + tspdf.wasm) that runs the reader-side tools entirely
# in the browser. `make wasm-test` drives merge/extract/rotate/encrypt/decrypt
# and friends through the module under node and validates every output with
# qpdf --check. `make wasm-demo` assembles the static in-browser demo site
# into wasm/demo/dist/ (deployable to GitHub Pages).
#
# These targets are intentionally NOT part of `test-all` or `check`: emcc is
# an optional build-time tool and its absence must not break the offline
# compiler-and-make-only gate.
EMCC ?= emcc
WASM_DIST = wasm/dist
# EXPORTED_FUNCTIONS only needs malloc/free: the shim's own entry points are
# kept alive by EMSCRIPTEN_KEEPALIVE. The runtime methods are what the JS
# wrapper (wasm/tspdf-api.js) touches. STACK_SIZE is raised because the PDF
# object parser and deep-copy recurse on untrusted structure depth.
WASM_FLAGS = -O2 -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createTspdf \
	-sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web,worker,node \
	-sSTACK_SIZE=1048576 \
	-sEXPORTED_FUNCTIONS=_malloc,_free \
	-sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32,UTF8ToString,stringToUTF8,lengthBytesUTF8

.PHONY: wasm wasm-test wasm-demo

wasm: $(WASM_DIST)/tspdf.js

$(WASM_DIST)/tspdf.js: wasm/shim.c $(ALL_SOURCES)
	@mkdir -p $(WASM_DIST)
	$(EMCC) $(REQUIRED_CFLAGS) $(WASM_FLAGS) -o $@ wasm/shim.c $(ALL_SOURCES) -lm

wasm-test: wasm $(CLI_TARGET)
	bash wasm/test/run_wasm_test.sh

wasm-demo: wasm
	python3 wasm/demo/build_demo.py
