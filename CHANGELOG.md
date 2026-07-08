# Changelog

All notable changes to tspdf are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project aims
to follow [Semantic Versioning](https://semver.org/) once it reaches 1.0. While
on 0.x, the CLI is considered stable but the low-level C API may still change.

## [Unreleased]

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
