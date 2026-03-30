# tspdf

PDF toolkit in pure C. Zero dependencies. Read, write, merge, encrypt — everything local.

- **No external libraries** — not even zlib. Builds anywhere, including WASM.
- **Read and manipulate existing PDFs** — merge, split, rotate, encrypt, watermark, annotate
- **Generate PDFs from scratch** — flexbox layout engine with tables, text wrapping, pagination
- **Full Unicode** via CIDFont Type2 + Identity-H
- **AES-128/256 encryption** with password support
- ~12K lines of C11.

Created by [Benjamin Lang](https://github.com/be-lang). Built in collaboration with [Claude Code](https://claude.ai/claude-code).

## Install

```bash
git clone https://github.com/be-lang/tspdf
cd tspdf
make && make install PREFIX=~/.local
```

That's it. Requires a C compiler and `make` — nothing else.

Use `sudo make install` to install system-wide to `/usr/local` instead. Uninstall with `make uninstall PREFIX=~/.local` (or `sudo make uninstall`).

## CLI

```bash
tspdf merge a.pdf b.pdf -o combined.pdf
tspdf split report.pdf --pages 1-5 -o extract.pdf
tspdf rotate doc.pdf --angle 90 -o rotated.pdf
tspdf delete doc.pdf --pages 2,4 -o trimmed.pdf
tspdf reorder doc.pdf --order 3,1,2 -o reordered.pdf
tspdf encrypt doc.pdf -o locked.pdf --password secret
tspdf decrypt locked.pdf -o unlocked.pdf --password secret
tspdf metadata doc.pdf                                    # view
tspdf metadata doc.pdf --set title="My Doc" -o out.pdf    # edit
tspdf info doc.pdf
tspdf watermark doc.pdf -o draft.pdf --text "DRAFT"
tspdf compress doc.pdf -o smaller.pdf
tspdf img2pdf photo1.jpg photo2.png -o album.pdf
tspdf md2pdf notes.md -o notes.pdf
tspdf qrcode "https://example.com" -o link.pdf
```

Run `tspdf help <command>` for detailed usage.

## Web UI

```bash
tspdf serve              # starts at http://localhost:8080
tspdf serve --port 3000  # custom port
```

Opens a local web interface for all PDF operations. Everything runs on your machine — no data leaves localhost. The UI is embedded in the binary; no extra files needed.

## Library Quick Start

```c
#include "include/tspdf.h"

static double measure_cb(const char *font, double size, const char *text, void *ud) {
    return tspdf_writer_measure_text((TspdfWriter *)ud, font, size, text);
}

int main(void) {
    TspdfWriter *doc = tspdf_writer_create();

    const char *font = tspdf_writer_add_builtin_font(doc, "Helvetica");

    TspdfArena arena = tspdf_arena_create(1024 * 1024);
    TspdfLayout ctx = tspdf_layout_create(&arena);
    ctx.measure_text = measure_cb;
    ctx.measure_userdata = doc;
    ctx.doc = doc;

    TspdfNode *root = tspdf_layout_box(&ctx);
    root->width = tspdf_size_fixed(595);
    root->height = tspdf_size_fixed(842);
    root->direction = TSPDF_DIR_COLUMN;
    root->padding = tspdf_padding_all(40);
    root->gap = 10;

    TspdfNode *title = tspdf_layout_text(&ctx, "Hello, PDF!", font, 24);
    tspdf_layout_add_child(root, title);

    TspdfStream *page = tspdf_writer_add_page(doc);
    tspdf_layout_compute(&ctx, root, 595, 842);
    tspdf_layout_render(&ctx, root, page);

    tspdf_writer_save(doc, "hello.pdf");
    tspdf_writer_destroy(doc);
    tspdf_arena_destroy(&arena);
}
```

```bash
gcc -o hello hello.c src/**/*.c -lm && ./hello
```

## PDF Manipulation

```c
#include "include/tspdf.h"

// Open, extract pages 1-3, save
TspdfError err;
TspdfReader *doc = tspdf_reader_open_file("input.pdf", &err);
size_t pages[] = {0, 1, 2};
TspdfReader *extract = tspdf_reader_extract(doc, pages, 3, &err);
tspdf_reader_save(extract, "pages_1_3.pdf");

// Merge two PDFs
TspdfReader *docs[] = {doc1, doc2};
TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
tspdf_reader_save(merged, "merged.pdf");

// Encrypt with AES-256
tspdf_reader_save_encrypted(doc, "locked.pdf", "password", "owner", 0, 256);
```

## Features

**PDF Reading & Manipulation** — open existing PDFs, extract/delete/rotate/reorder/merge pages. Add watermarks, annotations (links, text notes, stamps), and page numbers. Content overlay for drawing on existing pages. AES-128/256 encryption and decryption.

**PDF Generation** — flexbox-style layout with fixed, grow, fit-content, and percentage sizing. Automatic page breaks with repeating headers.

**Text** — TrueType font parsing and embedding with automatic subsetting. Full Unicode support. Word/character wrapping, alignment, decorations, inline rich text spans.

**Tables** — auto-sized columns, colspan support, alternating row colors, header/data styling.

**Drawing** — rounded corners, borders, shadows, backgrounds, opacity, clipping, transforms.

**Vector Paths** — move, line, cubic Bezier, arc, close. Fill and/or stroke.

**Gradients** — linear and radial, multi-stop.

**Images** — JPEG pass-through, PNG decoding from scratch.

**Forms** — text inputs, checkboxes.

**Compression** — deflate/inflate from scratch (RFC 1950/1951).

**QR Codes** — generate QR code PDFs from text or URLs.

**Markdown to PDF** — convert Markdown files to styled PDFs.

**Image to PDF** — convert JPEG/PNG images to PDF documents.

## Architecture

```
src/
  pdf/         PDF writing (objects, xref, streams, writer API)
  reader/      PDF reading and manipulation
  layout/      Flexbox layout engine, tables, lists, vector paths
  font/        TrueType parser, font subsetting
  image/       JPEG embedding, PNG decoder
  compress/    Deflate compressor/decompressor
  crypto/      AES, MD5, SHA-256, RC4
  qr/          QR code generation
  util/        Arena allocator, buffer
include/
  tspdf.h          Unified public header
  tspdf_overlay.h  Content overlay API
```

## License

MIT License. See [LICENSE](LICENSE).
