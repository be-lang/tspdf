# Changelog

All notable changes to tspdf are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project aims
to follow [Semantic Versioning](https://semver.org/) once it reaches 1.0. While
on 0.x, the CLI is considered stable but the low-level C API may still change.

## [Unreleased]

## [0.2.0] - 2026-07-09

### Added
- `tspdf text`: text extraction in content-stream order, with ToUnicode CMaps,
  /Differences, standard encodings, CID/Identity-H fonts, and form XObjects.
  Also available as `tspdf_reader_page_text()` in the C API.
- `tspdf pagenum`: stamp page numbers via the overlay path, with format,
  position, start number, and font size options.
- `split` with no `--pages` now bursts every page into its own zero-padded
  file.
- md2pdf renders GitHub-style pipe tables, block-level `![alt](path)` images,
  and `*italic*`/`_italic_`; long documents no longer silently truncate (a
  warning is printed past the 1024-block cap).
- `serve --bind <addr>` (default stays loopback) plus a Dockerfile; binding a
  non-loopback address prints a security warning.
- WASM build of the reader tools (`make wasm`), an in-browser demo site
  (`make wasm-demo`) deployed via GitHub Pages, and byte-identical
  save-to-memory guarantees.
- Versioned shared library with SONAME (`make shared`, installed by
  `install-lib`); pre-1.0 the SONAME tracks the minor version.
- Single-file amalgamation (`make amalgamation` produces `tspdf.c`/`tspdf.h`).
- Packaging: AUR PKGBUILD and Homebrew formula under `packaging/`, plus
  `docs/library.md` documenting the C API entry points.

### Changed
- merge and split preserve bookmarks (outlines) and AcroForm form fields;
  split keeps only entries pointing at kept pages and flattens named
  destinations to explicit ones.
- The shared library exports only `tspdf_`/`tspr_`-prefixed symbols.
- `-lm` moved to `Libs.private` in tspdf.pc (static consumers use
  `pkg-config --static`).

### Fixed
- Text extraction is hardened against hostile PDFs: NUL codepoints from
  ToUnicode no longer truncate output, wide-bfrange CMaps with point fixes
  resolve correctly, and total page content is capped.
- Outline and AcroForm handling is bounded against cyclic name trees, cyclic
  field /Kids, and cyclic outline sibling chains (previously could hang or
  emit still-cyclic trees).
- The web UI watermark tool honors the entered text and font size (previously
  always stamped DRAFT at 48pt), and the password tool honors a distinct
  owner password.

## [0.1.0] - 2026-07-08

First public release: a local-first, zero-dependency PDF toolkit with a CLI, an
embedded web UI, and a C library. Everything (deflate, crypto, PNG, TrueType,
layout) is implemented from scratch, with no external libraries.

### Command line and web UI
- Commands: merge, split, rotate, delete, reorder, encrypt, decrypt, metadata,
  info, watermark, compress, img2pdf, qrcode, md2pdf, and a local `serve` web UI.

### Reading
- Cross-reference tables and streams, hybrid-reference files, object streams,
  and incremental updates.
- Stream filters: Flate (with PNG/TIFF predictors), LZW, ASCIIHex, ASCII85,
  RunLength, and filter chains.
- Encryption: RC4 (R2 to R4), AES-128 (R4), and AES-256 (R6, ISO 32000-2), so
  PDFs from modern Acrobat, LibreOffice, and Microsoft Office open.
- Damaged-file recovery: rebuilds a missing or truncated cross-reference table
  or trailer by scanning for objects, tolerates junk before `%PDF-` and after
  `%%EOF`, and repairs near-miss `startxref` offsets.

### Generation
- Flexbox-style layout engine with fixed, grow, fit-content, and percentage
  sizing; automatic page breaks with repeating headers; tables with auto-sized
  columns and colspan.
- TrueType parsing and embedding with subsetting, Unicode via CIDFont Type2 +
  Identity-H, wrapping, alignment, and inline rich text.
- Graphics: borders, shadows, opacity, clipping, transforms, vector paths, and
  linear/radial gradients.
- Media: JPEG pass-through, PNG decoding (8-bit grayscale/RGB/RGBA,
  non-interlaced), QR codes, and Markdown to PDF.

### Library
- Installable via `make install`: `libtspdf.a`, relocatable headers under
  `<tspdf/...>`, and a pkg-config file. Headers are `extern "C"` guarded for
  C++ consumers.

### Testing
- Unit, reader, crypto, and CLI/web test suites; AddressSanitizer +
  UndefinedBehaviorSanitizer + LeakSanitizer builds; libFuzzer harnesses for the
  reader, deflate, PNG, and TrueType parsers; and an external conformance gate
  that validates output against `qpdf --check` and `mutool`.

[Unreleased]: https://github.com/be-lang/tspdf/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/be-lang/tspdf/releases/tag/v0.1.0
