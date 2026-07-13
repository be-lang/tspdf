# sources.mk — single source of truth for all library file lists.
# Makefile does `include sources.mk`; scripts/amalgamate.sh parses this file.
# One file per line; keep the same order as the previous Makefile source lists.
# Note: no blank lines inside a variable block; SRCDIR ?= is for make only
# (the amalgamate parser substitutes "src" itself).

SRCDIR ?= src

LIB_SOURCES = \
	$(SRCDIR)/util/buffer.c \
	$(SRCDIR)/util/arena.c \
	$(SRCDIR)/util/pdftext.c \
	$(SRCDIR)/util/pdfdate.c \
	$(SRCDIR)/pdf/primitives.c \
	$(SRCDIR)/pdf/pdf_writer.c \
	$(SRCDIR)/pdf/pdf_stream.c \
	$(SRCDIR)/pdf/pdf_base14.c \
	$(SRCDIR)/pdf/tspdf_writer.c \
	$(SRCDIR)/font/ttf_parser.c \
	$(SRCDIR)/font/font_subset.c \
	$(SRCDIR)/font/font_fallback.c \
	$(SRCDIR)/layout/layout.c \
	$(SRCDIR)/image/jpeg_embed.c \
	$(SRCDIR)/image/jpeg_codec.c \
	$(SRCDIR)/image/ccitt_codec.c \
	$(SRCDIR)/image/png_decoder.c \
	$(SRCDIR)/compress/deflate.c \
	$(SRCDIR)/filters/filters.c \
	$(SRCDIR)/qr/qr_encode.c \
	$(SRCDIR)/tspdf_error.c

TSPR_SOURCES = \
	$(SRCDIR)/reader/tspr_parser.c \
	$(SRCDIR)/reader/tspr_xref.c \
	$(SRCDIR)/reader/tspr_pages.c \
	$(SRCDIR)/reader/tspr_serialize.c \
	$(SRCDIR)/reader/tspr_infoplan.c \
	$(SRCDIR)/reader/tspr_crypt.c \
	$(SRCDIR)/reader/tspr_document.c \
	$(SRCDIR)/reader/tspr_doctree.c \
	$(SRCDIR)/reader/tspr_attach.c \
	$(SRCDIR)/reader/tspr_bookmark.c \
	$(SRCDIR)/reader/tspr_metadata.c \
	$(SRCDIR)/reader/tspr_xmp.c \
	$(SRCDIR)/reader/tspr_content.c \
	$(SRCDIR)/reader/tspr_import.c \
	$(SRCDIR)/reader/tspr_nup.c \
	$(SRCDIR)/reader/tspr_resources.c \
	$(SRCDIR)/reader/tspr_annot.c \
	$(SRCDIR)/reader/tspr_form.c \
	$(SRCDIR)/reader/tspr_text.c \
	$(SRCDIR)/reader/tspr_lossy.c

CRYPTO_SOURCES = \
	$(SRCDIR)/crypto/md5.c \
	$(SRCDIR)/crypto/sha256.c \
	$(SRCDIR)/crypto/sha512.c \
	$(SRCDIR)/crypto/rc4.c \
	$(SRCDIR)/crypto/aes.c

# Public headers (umbrella closure, dependency order).
PUBLIC_HEADERS = \
	include/tspdf/version.h \
	src/tspdf_error.h \
	src/util/buffer.h \
	src/util/arena.h \
	src/font/ttf_parser.h \
	src/font/font_subset.h \
	src/image/jpeg_embed.h \
	src/pdf/pdf_writer.h \
	src/pdf/pdf_base14.h \
	src/pdf/pdf_stream.h \
	src/pdf/tspdf_writer.h \
	src/reader/tspr.h \
	src/reader/tspr_overlay.h \
	src/layout/layout.h

# Implementation-only headers (deps satisfied by tspdf.h), dependency order.
INTERNAL_HEADERS = \
	src/pdf/primitives.h \
	src/compress/deflate.h \
	src/filters/filters.h \
	src/crypto/md5.h \
	src/crypto/sha256.h \
	src/crypto/sha512.h \
	src/crypto/rc4.h \
	src/crypto/aes.h \
	src/image/png_decoder.h \
	src/image/jpeg_codec.h \
	src/image/ccitt_codec.h \
	src/qr/qr_encode.h \
	src/util/pdftext.h \
	src/util/pdfdate.h \
	src/font/font_fallback.h \
	src/reader/tspr_internal.h \
	src/reader/tspr_doctree.h \
	src/reader/tspr_attach.h \
	src/reader/tspr_text.h

# Umbrella headers: known but not amalgamated (they only #include the lists above).
UMBRELLA_HEADERS = \
	include/tspdf.h \
	include/tspdf_overlay.h
