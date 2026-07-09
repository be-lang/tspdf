# tspdf

[![CI](https://github.com/be-lang/tspdf/actions/workflows/ci.yml/badge.svg)](https://github.com/be-lang/tspdf/actions/workflows/ci.yml)

A local-first PDF toolkit in pure C. Merge, split, encrypt, watermark, compress,
and more, all on your machine. No uploads, no cloud, no dependencies, not even
zlib. Just a single binary.

![tspdf web UI](docs/webui-screenshot.png)

## Install

Needs a C11 compiler and `make`, nothing else.

```bash
git clone https://github.com/be-lang/tspdf
cd tspdf
make
make install PREFIX=~/.local        # user-local; or `sudo make install` for /usr/local
```

Or grab a prebuilt binary from the [releases](https://github.com/be-lang/tspdf/releases) page.

Uninstall with `make uninstall PREFIX=~/.local`.

## Command line

```bash
tspdf merge a.pdf b.pdf -o combined.pdf
tspdf split report.pdf --pages 1-5 -o extract.pdf
tspdf encrypt doc.pdf -o locked.pdf --password secret
tspdf decrypt locked.pdf -o unlocked.pdf --password secret
tspdf rotate doc.pdf --angle 90 -o rotated.pdf
tspdf watermark doc.pdf -o draft.pdf --text "DRAFT"
tspdf pagenum doc.pdf -o numbered.pdf
tspdf compress doc.pdf -o smaller.pdf
tspdf text doc.pdf -o doc.txt
tspdf metadata doc.pdf
tspdf md2pdf notes.md -o notes.pdf
tspdf img2pdf photo.jpg -o photo.pdf
tspdf qrcode "https://example.com" -o qr.pdf
```

Run `tspdf help <command>` for details. See the [cookbook](docs/cookbook.md) for
more examples.

## Web UI

```bash
tspdf serve                 # http://localhost:8080
tspdf serve --port 3000
```

A local web interface for the same operations. The UI is embedded in the binary,
so there are no extra files and no network access. Your documents stay on your
computer.

You can also try it in the browser at <https://be-lang.github.io/tspdf/>. That
is the same code compiled to WebAssembly, so files never leave the page.

## PDF compatibility

tspdf aims to open the PDFs you actually have, not just clean ones:

- Cross-reference tables and streams, hybrid-reference files, object streams, and incremental updates
- Filters: Flate (with PNG/TIFF predictors), LZW, ASCIIHex, ASCII85, RunLength, and filter chains
- Encryption: RC4 (R2 to R4), AES-128 (R4), and AES-256 (R6, ISO 32000-2), which is what modern Acrobat, LibreOffice, and Office produce
- Damaged files: recovers documents with a missing or truncated xref/trailer by scanning for objects, tolerates junk before `%PDF-` and after `%%EOF`

Output is checked against `qpdf` and `mutool` in CI. If a PDF fails to open,
please [open an issue](../../issues) with the file if you can share it. See
[known limitations](docs/known-limitations.md) for current gaps.

## Use as a C library

`make install` also installs `libtspdf.a`, a versioned `libtspdf.so`, headers
under `<tspdf/...>`, and a pkg-config file. `make amalgamation` produces a
single-file `tspdf.c` + `tspdf.h` to drop into a project. The headers are
`extern "C"` guarded for C++.
API overview, ownership rules, and examples: [docs/library.md](docs/library.md).

```bash
cc app.c $(pkg-config --cflags --libs tspdf) -o app
```

```c
#include <tspdf/tspdf.h>

TspdfError err;
TspdfReader *doc = tspdf_reader_open_file("input.pdf", &err);

size_t pages[] = {0, 1, 2};
TspdfReader *extract = tspdf_reader_extract(doc, pages, 3, &err);
tspdf_reader_save(extract, "pages_1_3.pdf");

tspdf_reader_save_encrypted(doc, "locked.pdf", "password", "owner", 0, 256);
```

The same library builds PDFs from scratch through the writer and layout engine
(see the headers under `<tspdf/pdf/>` and `<tspdf/layout/>`). Building against a
source checkout instead of an install? Use `#include "include/tspdf.h"` and link
the sources directly.

## Testing

```bash
make test-all           # unit + reader + crypto + CLI/web tests
make check              # clean -Werror rebuild + full suite (the CI gate)
make test-asan          # writer/crypto under ASan + UBSan + Leak
make test-asan-reader   # reader/parser under the same
make test-external      # round-trip output through qpdf --check and mutool
make fuzz               # libFuzzer harnesses for the reader/deflate/PNG/TTF parsers
```

CI runs `make check` across `{Linux, macOS} × {gcc, clang}`, plus the sanitizer
and conformance jobs, on every push.

## Features

- **Reading**: open PDFs, extract/delete/rotate/reorder/merge pages, text extraction, watermarks, annotations, page numbers, content overlay, AES-128/256 encryption and decryption.
- **Generation**: flexbox-style layout with fixed/grow/fit-content/percentage sizing, automatic page breaks with repeating headers, tables with auto-sized columns and colspan.
- **Text**: TrueType parsing and embedding with subsetting, Unicode via CIDFont Type2 + Identity-H, wrapping, alignment, inline rich text.
- **Graphics**: borders, shadows, opacity, clipping, transforms, vector paths, linear and radial gradients.
- **Media**: JPEG and PNG pass-through, PNG decoding (8-bit grayscale/RGB/RGBA and palette, non-interlaced), QR codes, Markdown to PDF.
- **From scratch**: deflate/inflate (RFC 1950/1951), AES-128/256, MD5, SHA-256/384/512, RC4.

## Credits

Created by [Benjamin Lang](https://github.com/be-lang). Built with
[Claude Code](https://claude.ai/claude-code).

## License

MIT. See [LICENSE](LICENSE).
