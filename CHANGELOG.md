# Changelog

All notable changes to tspdf are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project aims
to follow [Semantic Versioning](https://semver.org/) once it reaches 1.0. While
on 0.x, the CLI is considered stable but the low-level C API may still change.

## [Unreleased]

## [0.3.0] - 2026-07-10

### Added
- `tspdf form` — list, fill, and flatten AcroForm form fields. `form list`
  emits JSON; `form fill` takes `--data <json>` or repeated `--set name=value`
  (text, checkbox, radio, and choice fields); `form flatten` bakes values into
  the page and drops the interactive form.
- `tspdf attach` — embed, list, extract, and remove file attachments; attached
  files survive merge and split, and extraction sanitizes stored names.
- `tspdf stamp` — overlay one PDF's page onto another's pages, with `--pages`,
  `--under`, and `--stamp-page`.
- `tspdf watermark --image <file>` — image watermarks (PNG/JPEG), with
  `--opacity`, `--scale`, and positioning, honoring page rotation.
- `tspdf text --layout` — layout-preserving extraction that keeps columns and
  tables aligned (pdftotext -layout style).
- `tspdf nup <N>` — place multiple source pages per sheet (2-up, 4-up, ...),
  with grid, gap, frame, and page-size options.
- `tspdf crop` — set the CropBox (clip the visible region) by explicit box or
  margins; `tspdf scale` — resize pages to a named size or by a factor,
  scaling their content.
- `tspdf bookmark` — list, add, import (from a TAB-separated TOC file), and
  clear the outline of an existing PDF.
- `tspdf stamp --stamp-password` for encrypted stamp sources.
- `tspdf info --json` — machine-readable document facts.
- `tspdf encrypt --permissions <list>` — restrict allowed actions (print,
  copy, modify, annotate, forms, extract, assemble, print-hq).
- `tspdf qrcode --ec-level L|M|Q|H` — choose the QR error-correction level.
- `metadata --clear <key>` removes a field; `producer` is now settable.
- C API: `tspdf_reader_form_fields`/`_form_fill`/`_form_flatten`,
  `tspdf_reader_attachments`/`_attachment_get`/`_attachment_add`/`_remove`,
  `tspdf_reader_import_page_xobject`, `tspdf_reader_page_text_layout`.

### Changed
- merge and split preserve embedded-file attachments and `/PageLabels` (page
  numbering styles) in addition to bookmarks and form fields.
- `metadata` and `info` print PDF dates in a readable form
  (`2013-10-31 14:01:50 +04:00`); `info --json` also keeps the raw value.

## [0.2.0] - 2026-07-09

### Added
- `pagenum --pages <range>` stamps only the selected pages (skip cover pages);
  numbering still reflects true page positions.
- `img2pdf --page-size a4|letter|image` (`image` sizes each page to its image
  at 72 dpi, like python img2pdf).
- md2pdf: `[text](url)` becomes a real clickable link annotation, headings
  become nested PDF bookmarks, and `***bold-italic***` renders styled.
- `info` reports the PDF version, encryption scheme (e.g. "AES-256, R6"),
  and whether the file has bookmarks or form fields; four matching C API
  accessors (`tspdf_reader_pdf_version`, `tspdf_reader_encryption_info`,
  `tspdf_reader_has_outlines`, `tspdf_reader_has_acroform`).
- A deflate round-trip fuzz harness (`fuzz/fuzz_deflate.c`).
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
- AES encryption/decryption is 50-100x faster: the block cipher is now
  table-driven (pure C11), with hardware fast paths on x86 (AES-NI) and
  ARMv8 (crypto extensions) selected at runtime (portable fallback kept;
  `TSPDF_NO_AESHW=1` forces it).
  Decrypting a 23 MB AES-256 file drops from ~15 s to ~0.05 s. Correctness is
  pinned by FIPS-197 and NIST SP 800-38A vectors, both paths byte-compared,
  and key material is wiped before contexts are freed. `make bench-crypto`
  reports throughput.
- `compress` was overhauled and now actually compresses: the from-scratch
  DEFLATE encoder gained a lazy hash-chain matcher and dynamic Huffman blocks
  (now smaller than zlib level 6 on typical PDF payloads), and the serializer
  packs small objects into object streams with a right-sized cross-reference
  stream. The 23.7 MB reference file shrinks 9.8% (previously 0.4%), and
  object-stream-heavy inputs no longer grow. Everything tspdf writes (md2pdf,
  img2pdf, merge/split output) benefits from the better encoder.
- img2pdf embeds PNG image data verbatim (no recompression) for non-interlaced
  gray/RGB/palette PNGs and keeps their colorspace, instead of expanding to
  RGB and re-encoding — output is now typically smaller than python img2pdf's
  (previously up to 358x larger).
- `tspdf text` folds ligature code points (U+FB00-FB06) to ASCII sequences
  and trims trailing spaces, so extracted text is searchable.
- Extra positional arguments are now rejected with an error instead of being
  silently ignored.
- merge and split preserve bookmarks (outlines) and AcroForm form fields;
  split keeps only entries pointing at kept pages and flattens named
  destinations to explicit ones.
- The shared library exports only `tspdf_`/`tspr_`-prefixed symbols.
- `-lm` moved to `Libs.private` in tspdf.pc (static consumers use
  `pkg-config --static`).

### Fixed
- The QR encoder now produces spec-conformant, decode-verified symbols across
  all supported sizes (versions 1-11, up to 251 characters). Previously the
  module placement skipped a column, one format-info copy was bit-reversed,
  the v3/v10 block tables were wrong, and versions 7+ lacked the mandatory
  version-information block — strict decoders rejected most output, and
  27-42-character payloads (typical URLs) were always broken.
- merge and img2pdf accept any number of inputs; previously inputs past the
  64th were silently dropped.
- watermark centers on the visible page area (nonzero MediaBox origins) and
  reads upright on rotated pages, in both the CLI and the web UI.
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
