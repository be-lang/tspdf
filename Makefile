CC ?= gcc
CFLAGS = -Wall -Wextra -std=c11 -g -O2
SRCDIR = src
BUILDDIR = build
PREFIX ?= /usr/local

LIB_SOURCES = \
	$(SRCDIR)/util/buffer.c \
	$(SRCDIR)/util/arena.c \
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
	$(SRCDIR)/reader/tspr_metadata.c \
	$(SRCDIR)/reader/tspr_content.c \
	$(SRCDIR)/reader/tspr_resources.c \
	$(SRCDIR)/reader/tspr_annot.c

CRYPTO_SOURCES = \
	$(SRCDIR)/crypto/md5.c \
	$(SRCDIR)/crypto/sha256.c \
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
	cli/server.c

CLI_TARGET = $(BUILDDIR)/tspdf

.PHONY: all cli install uninstall clean test test-all test-cli test-reader test-crypto \
        lib demo bench bench-reader minimal reader-demo generate-test-pdfs

# --- Default: build the CLI ---

all: $(CLI_TARGET)

# Phony alias: a directory named `cli/` exists, so `make cli` must not be a no-op.
cli: $(CLI_TARGET)

$(CLI_TARGET): $(CLI_SOURCES) $(ALL_SOURCES)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $(CLI_SOURCES) $(ALL_SOURCES) -lm

# --- Install / uninstall ---

install: $(CLI_TARGET)
	install -d $(PREFIX)/bin
	install -m 755 $(CLI_TARGET) $(PREFIX)/bin/tspdf

uninstall:
	rm -f $(PREFIX)/bin/tspdf

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

$(BUILDDIR)/test_runner: tests/test_main.c $(LIB_SOURCES)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_SOURCES) -lm

$(BUILDDIR)/test_reader: tests/test_reader.c $(ALL_SOURCES)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(TSPR_SOURCES) $(LIB_SOURCES) $(CRYPTO_SOURCES) -lm

$(BUILDDIR)/test_crypto: tests/test_crypto.c $(CRYPTO_SOURCES)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(CRYPTO_SOURCES) -lm

# --- Library (for embedding in other projects) ---

lib: $(BUILDDIR)/libtspdf.a

$(BUILDDIR)/libtspdf.a: $(ALL_SOURCES)
	@mkdir -p $(BUILDDIR)/obj
	for f in $(ALL_SOURCES); do $(CC) $(CFLAGS) -c $$f -o $(BUILDDIR)/obj/$$(basename $$f .c).o; done
	ar rcs $@ $(BUILDDIR)/obj/*.o

# --- Examples & benchmarks ---

demo: $(BUILDDIR)/tspdf_demo
	./$(BUILDDIR)/tspdf_demo

$(BUILDDIR)/tspdf_demo: examples/demo.c $(LIB_SOURCES)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_SOURCES) -lm

minimal: $(BUILDDIR)/minimal
	./$(BUILDDIR)/minimal

$(BUILDDIR)/minimal: examples/minimal.c $(LIB_SOURCES)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_SOURCES) -lm

reader-demo: $(BUILDDIR)/reader_demo
	./$(BUILDDIR)/reader_demo

$(BUILDDIR)/reader_demo: examples/reader_demo.c $(ALL_SOURCES)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_SOURCES) $(TSPR_SOURCES) $(CRYPTO_SOURCES) -lm

bench: $(BUILDDIR)/bench
	./$(BUILDDIR)/bench

$(BUILDDIR)/bench: examples/benchmark.c $(LIB_SOURCES)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_SOURCES) -lm

bench-reader: $(BUILDDIR)/bench_reader
	./$(BUILDDIR)/bench_reader

$(BUILDDIR)/bench_reader: examples/bench_reader.c $(ALL_SOURCES)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_SOURCES) $(TSPR_SOURCES) $(CRYPTO_SOURCES) -lm

generate-test-pdfs: $(BUILDDIR)/generate_test_pdfs
	@mkdir -p tests/data
	./$(BUILDDIR)/generate_test_pdfs

$(BUILDDIR)/generate_test_pdfs: tests/generate_test_pdfs.c $(LIB_SOURCES)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_SOURCES) -lm
