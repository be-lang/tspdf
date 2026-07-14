# Changelog

All notable changes to tspdf are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project aims
to follow [Semantic Versioning](https://semver.org/) once it reaches 1.0. While
on 0.x, the CLI is considered stable but the low-level C API may still change.

## [Unreleased]

### Fixed
- Destroying a source reader before saving a document derived from it
  (extract/delete/rotate/reorder/crop/scale/resize/n-up, which alias the
  source's data until saved) is no longer a use-after-free: the library holds
  an internal reference from the derived document to its source and defers the
  source's free until the last derived document is destroyed. The source handle
  is still dead to the caller the moment `tspdf_reader_destroy` returns; only
  the memory lifetime is deferred. Save-then-destroy remains the recommended
  order.
- Web-demo metadata edits now update the XMP packet (previously the wasm shim
  called Info setters directly without syncing the XMP stream, leaving the
  packet stale for viewers that prefer XMP over the Info dictionary).

## [0.5.0] - 2026-07-14

### Changed
- `metadata --set/--clear` now updates the XMP packet too, not just the Info
  dictionary, so viewers that prefer XMP (Acrobat) show the new values:
  title, author, subject, keywords, creator, and producer map to dc:title,
  dc:creator, dc:description, pdf:Keywords, xmp:CreatorTool, and
  pdf:Producer, and xmp:ModifyDate is refreshed with the same timestamp the
  save writes into Info /ModDate. Editing is conservative — a property
  already in the packet gets its value replaced (XML-escaped, keeping the
  xpacket padding contract); nothing is injected. The stale-XMP notice now
  fires only for edited fields whose property the packet does not carry, or
  for packets that cannot be edited safely (UTF-16, multi-author
  dc:creator).

### Fixed
- Writer now escapes PDF names and string control bytes per spec (ISO 32000
  §7.3.4/§7.3.5). Previously `tspdf_raw_write_name` emitted names verbatim
  (bytes needing `#HH` encoding were written raw), and `tspdf_raw_write_string`
  omitted the `\n \r \t \b \f` named escapes and the `\NNN` octal fallback for
  other control bytes. Both functions now delegate to the new canonical encoder
  in `src/pdf/primitives.c`, shared with the reader serializer.
- Every command that reads a PDF now takes `--password`/`--password-file`:
  `rotate`, `delete`, `reorder`, `split`, `merge`, `crop`, `scale`, `pagenum`,
  `watermark`, and `compress` gained both (they could not open encrypted files
  at all); `text`, `info`, and `form` gained `--password-file`. Outputs keep
  the original encryption; `merge` tries the one password on every input.
- `encrypt` no longer drops the document's Info metadata (Title, Author, ...):
  the existing Info dict is carried into the encrypted output with its strings
  encrypted, and `decrypt` round-trips it. Plain saves that only refresh the
  Producer stamp also keep the other Info fields now.
- `attach list` reports the real decoded size of an attachment instead of
  trusting the embedded /Params /Size, which a crafted file can fake. /Params
  is only used when the stream cannot be decoded.
- The two halves of the trailer /ID now differ on encrypted saves, as the spec
  intends for updated files. The first half is unchanged (it is key material
  for RC4/AES-128 files); the second is derived from the output bytes, so
  saves stay deterministic.
- CCITT decoder: the one-time lookup-table setup is now thread-safe (C11
  atomics) instead of relying on a benign data race.
- `text`: super- and subscripts no longer break the line. A run whose
  baseline stays within 0.5 em of the line's first run (poppler's
  tolerance) joins it, so `1.0·10` + `20` comes out as `1.0·1020` and
  footnote markers stay attached (`sampling,13 misalignment-related`).
  At such a boundary a gap wider than 0.1 em of the smaller script still
  becomes a space (`QK T`), and a raised marker that starts a line keeps
  its own line (footnote blocks), both matching pdftotext.
- `text --layout`: rows are built from content-order lines merged by
  baseline, so table cells whose columns use slightly different leading
  are no longer split into staggered rows (the syscard
  `Top 3 Correlated Emotion Representations` block), and the blank-line
  gap below a merged row is measured from its lowest baseline so the
  merge cannot open a spurious gap.
- Web demo / wasm watermark now honors page rotation: on pages with a
  `/Rotate` entry the watermark text is pre-rotated so it reads upright
  as the viewer displays the page, matching the behavior of `tspdf watermark`.

## [0.4.0] - 2026-07-11

### Added
- `form fill` / `form flatten`: values with characters outside WinAnsi (CJK,
  Greek, ...) now render in the generated appearances instead of `?`. A
  TrueType fallback font is discovered at run time (`TSPDF_FALLBACK_FONT=
  /path/to/font.ttf` override, else a scan of the system font directories
  with a real coverage check; `TSPDF_FONT_DIRS` replaces the scan roots),
  subset to the needed glyphs, and embedded as a CIDFontType2/Identity-H
  font with /ToUnicode. `.ttc` collections with TrueType outlines are
  supported; CFF-flavored fonts are skipped. Without a usable font the old
  `?` rendering and warning remain. C API:
  `tspdf_reader_form_value_renderable`.
- `tspdf compress --lossy`: downsample oversized photos/scans to a target
  resolution (`--image-dpi`, default 150) and re-encode them as JPEG
  (`--image-quality`, default 75), using a from-scratch baseline JPEG codec.
  Rendered image sizes are measured from the page content streams, near-gray
  RGB scans are converted to grayscale, and anything unusual (transparency,
  ICC/Indexed color, progressive JPEG input) passes through untouched.
  Without `--lossy`, compress output is unchanged. C API:
  `tspdf_reader_lossy_images`.
- `compress --lossy` now also recompresses black-and-white images (scanned
  books): 1-bit CCITT fax or Flate images are downsampled to `--mono-dpi`
  (default 300; text needs more dpi than photos to stay readable) and
  re-encoded as CCITT G4 with a from-scratch G3/G4 codec. Originals are kept
  unless the new stream is at least 10% smaller.
- `tspdf bookmark import --append`: add the imported TOC after the existing
  outline instead of replacing it.
- `tspdf split --no-attachments`: drop embedded files from the outputs (by
  default every part gets a copy of each attachment).
- `tspdf attach add` records each file's size, modification time, an MD5
  checksum, and a MIME type from the file extension (`--mime` overrides);
  `attach list --json` reports the type. C API:
  `tspdf_reader_attachment_add_ex`.
- `tspdf stamp` accepts `--password-file` and `--stamp-password-file`, so
  passwords for encrypted inputs and stamp files stay out of argv.
- `tspdf metadata` accepts `--password`/`--password-file` to view and edit
  encrypted files; edited output keeps the original encryption.

### Changed
- `tspdf compress` output got smaller. A new best-effort deflate level
  (zlib-9-class, used only by compress) plus re-encoding of
  ASCII85/ASCIIHex/LZW-armored streams as plain Flate beat qpdf on all four
  benchmark files, e.g. a 125-page text PDF: 280 KB before, 223 KB now
  (qpdf: 224 KB).
- Saving a file that uses object streams re-packs them instead of unpacking
  every object (cropping f1040.pdf: 460 KB before, 216 KB now). Classic files
  are written classic as before. Encrypted saves re-pack too.
- `nup` and `stamp` reuse resources already imported into the output instead
  of copying them once per sheet: 4-up of a 2.2 MB paper is 2.1 MB, down
  from 3.2 MB.
- Saving a document opened with a password now keeps it encrypted (same
  passwords and permissions), matching qpdf. This covers form fill/flatten,
  bookmark, attach, stamp, nup, metadata, rotate, and the C API; `tspdf
  decrypt` (and the web UI's unlock) remain the explicit way to remove
  encryption. C API: new `TspdfSaveOptions.decrypt` opt-out.
- `tspdf info` reports decoded permissions and the raw /P value on encrypted
  files, and shows the CropBox when it differs from the MediaBox; `info
  --json` failures now emit a JSON error object instead of plain text.

### Fixed
- `bookmark add` (and `import --append`) rewrote every existing outline entry
  to a plain page destination; scroll position/zoom, other view types,
  actions, colors, styles, and collapsed state are now preserved.
- `metadata --set/--clear` silently left an XMP metadata stream stale; it now
  prints a notice, since viewers preferring XMP may show the old values.
- `form fill` skipped /Opt validation for choice fields whose option list
  lives on an ancestor field (/Opt is inheritable): unknown values were
  silently accepted, and `form list` showed no options for such fields.
- `watermark` and `pagenum` corrupted pages whose /Resources is an indirect
  reference (common in pdfTeX and PyMuPDF output): a duplicate /Resources key
  made strict viewers drop the page's fonts.
- `merge` wrote invalid xref entries and blanked bookmark titles when outline
  /Title strings are indirect (pdfTeX/hyperref); `bookmark list` showed those
  titles as empty and `bookmark add` failed outright on such files.
- Text extraction missed inter-word spaces at font-change boundaries
  (italic/math in TeX documents): "arXivpreprintarXiv" → "arXiv preprint
  arXiv". Word recall on a typical arXiv paper: 96.4% → 99.0%.
- Attachment names, bookmark titles, and metadata now follow the PDF
  text-string rules (ISO 32000 §7.9.2.2): non-ASCII attachment names are
  written as UTF-16BE (readable by qpdf/PyMuPDF instead of mojibake), and
  UTF-16BE / PDFDocEncoding strings from other tools decode correctly —
  `attach extract --all` no longer fails on them.
- `form fill` validates choice values against the field's options (free text
  still allowed for editable combos) and warns when a value contains
  characters the appearance font cannot show.
- `scale --to` preserved page size but flipped the viewed orientation of
  rotated (/Rotate 90/270) pages.
- Re-encrypting a file with new passwords no longer embeds the old /Encrypt
  dictionary (old password verifiers) as an orphan object.
- Metadata written into encrypted files was stored as plaintext; readers
  decrypt Info strings unconditionally, so poppler showed garbage (blank
  titles). Info strings are now encrypted like every other string.

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
