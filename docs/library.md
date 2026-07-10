# Using tspdf as a C library

`make install` (or `make install-lib` for just the library) installs `libtspdf.a`,
a versioned `libtspdf.so`, the headers under `<tspdf/...>`, and a pkg-config file:

```bash
cc app.c $(pkg-config --cflags --libs tspdf) -o app
```

That links the shared library; to link `libtspdf.a` instead, use
`pkg-config --static --libs tspdf` (which adds `-lm`).

There is also a single-file form: `make amalgamation` generates
`build/amalgamation/tspdf.c` + `tspdf.h`. Drop both into your project and compile
`tspdf.c` as C11; releases after v0.1.0 ship them prebuilt as a tarball.

## Supported surface

Everything comes in through `#include <tspdf/tspdf.h>`. The most-used calls:

Reader — parse, transform, save existing PDFs (`<tspdf/reader/tspr.h>`):

- `tspdf_reader_open_file` / `tspdf_reader_open` — parse a PDF from a file or from memory
- `tspdf_reader_page_count` / `tspdf_reader_get_page` — page count and per-page box/rotation
- `tspdf_reader_extract` / `tspdf_reader_delete` / `tspdf_reader_rotate` / `tspdf_reader_reorder` — page operations; each returns a new document, the source is unchanged
- `tspdf_reader_merge` — combine documents into a new, self-contained one
- `tspdf_reader_save` / `tspdf_reader_save_to_memory` / `tspdf_reader_save_encrypted` — write out (optionally RC4/AES encrypted)
- `tspdf_reader_page_text` — extract a page's text as UTF-8
- `tspdf_reader_import_page_xobject` / `tspdf_page_add_xobject` — wrap another document's page as a form XObject and draw it on pages (what `tspdf stamp` uses); the import is self-contained, so the source document may be destroyed right after
- `tspdf_reader_pdf_version` / `tspdf_reader_encryption_info` / `tspdf_reader_has_outlines` / `tspdf_reader_has_acroform` — document facts
- `tspdf_reader_destroy` — free a document and everything it returned

Writer — build PDFs from scratch (`<tspdf/pdf/tspdf_writer.h>`):

- `tspdf_writer_create` / `tspdf_writer_destroy`
- `tspdf_writer_add_page` — appends a page and returns its content stream
- `tspdf_writer_add_builtin_font` / `tspdf_writer_add_ttf_font` — base-14 fonts, or embed + subset a TTF
- `tspdf_writer_add_png_image` / `tspdf_writer_add_jpeg_image`
- `tspdf_writer_add_link` / `tspdf_writer_add_bookmark` / `tspdf_writer_add_bookmark_xyz` — URL link annotations and (nested) outline entries
- `tspdf_writer_save` / `tspdf_writer_save_to_memory`

Content streams — draw on a page (`<tspdf/pdf/pdf_stream.h>`): `tspdf_stream_begin_text`,
`tspdf_stream_set_font`, `tspdf_stream_text_position`, `tspdf_stream_show_text`,
`tspdf_stream_end_text`, `tspdf_stream_rect`, `tspdf_stream_fill`, `tspdf_stream_stroke`,
`tspdf_stream_draw_image`.

Layout — flexbox-style engine over the writer (`<tspdf/layout/layout.h>`):
`tspdf_layout_create`, `tspdf_layout_box`, `tspdf_layout_text`, `tspdf_layout_add_child`,
`tspdf_layout_compute`, `tspdf_layout_render`, `tspdf_layout_tree_free`.
See `examples/minimal.c` for the full pattern.

## Stability

Pre-1.0: a minor release (0.x) may change or remove API; a patch release (0.x.y)
will not. Anything not listed above (internal headers, `tspr_internal.h`, the
`tspdf_pdf_*` low-level writer) can change at any time.

## Ownership and lifetimes

- Reader results are arena-owned. Strings, objects, and pages returned by a
  `TspdfReader` live in that reader's arena and are freed by
  `tspdf_reader_destroy`. Don't free them yourself; copy what you need to keep.
- Extract/delete/rotate/reorder return a new document and the source is
  unchanged. The source document must outlive the returned document unless
  saved first, because the returned document references the source's stream
  data. Merge copies stream data, so merged documents are self-contained.
- `tspdf_reader_save_to_memory` and `tspdf_writer_save_to_memory` malloc the
  output buffer; the caller frees it.

## Reader example

```c
#include <tspdf/tspdf.h>
#include <stdio.h>

int main(void) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("input.pdf", &err);
    if (!doc) { fprintf(stderr, "open: %s\n", tspdf_error_string(err)); return 1; }
    printf("%zu pages\n", tspdf_reader_page_count(doc));

    size_t pages[] = {0};
    TspdfReader *first = tspdf_reader_extract(doc, pages, 1, &err);
    if (!first || tspdf_reader_save(first, "first_page.pdf") != TSPDF_OK) return 1;

    tspdf_reader_destroy(first);   /* destroy the result before its source */
    tspdf_reader_destroy(doc);
    return 0;
}
```

## Writer example

```c
#include <tspdf/tspdf.h>

int main(void) {
    TspdfWriter *doc = tspdf_writer_create();
    const char *font = tspdf_writer_add_builtin_font(doc, "Helvetica");

    TspdfStream *page = tspdf_writer_add_page(doc);
    tspdf_stream_begin_text(page);
    tspdf_stream_set_font(page, font, 24);
    tspdf_stream_text_position(page, 72, 720);
    tspdf_stream_show_text(page, "Hello from tspdf");
    tspdf_stream_end_text(page);

    TspdfError err = tspdf_writer_save(doc, "hello.pdf");
    tspdf_writer_destroy(doc);
    return err == TSPDF_OK ? 0 : 1;
}
```

Both compile against an installed tree with `cc -std=c11 example.c -ltspdf -lm`.

## Limitations

The shared library exports only `tspdf_*`/`tspr_*` symbols (a linker export
list hides the internal helpers); the static library and the amalgamation have
no such filter, so treat only the `tspdf_*` names documented here as API.
No thread-safety guarantees: use one `TspdfReader`/`TspdfWriter` per thread.
The writer has fixed compile-time capacities (fonts, images, bookmarks, form
fields — the `TSPDF_MAX_*` constants at the top of `<tspdf/pdf/tspdf_writer.h>`).
See [known-limitations.md](known-limitations.md) for format-level gaps.
