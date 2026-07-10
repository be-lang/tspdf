# Known Limitations

This file tracks current, known constraints so users can evaluate fit quickly.

See the [compatibility matrix](compatibility-matrix.md) for what is tested where.

## Scope

- Text extraction (`tspdf text`) reads text in content-stream order: no column re-ordering, no OCR of scanned pages, and no blank-line preservation. CID fonts without a /ToUnicode map extract as U+FFFD (a stderr warning names the affected pages). Ligature code points (U+FB00–FB06) are folded to ASCII letter sequences (ff, fi, fl, ...) for searchability, matching Unicode NFKC and poppler's Latin1 fold tables — note that default pdftotext (UTF-8 output) keeps the raw code points. Trailing spaces are trimmed from each line, as pdftotext does.
- No rendering of pages to images.
- `form fill` renders generated appearances with a WinAnsi (cp1252) base font: characters outside it (CJK, Greek, ...) display as `?` in the widget and in flattened output. The stored field value is intact and a stderr warning names the field. No fallback-font embedding yet.
- md2pdf renders pipe tables and block-level `![alt](path)` images. Inline bold/italic/bold-italic/code only keeps its styling when the line fits unwrapped; longer lines render plain (markers stripped). `[text](url)` becomes a clickable link annotation under the same condition — on lines that wrap, the link renders as plain text without an annotation. Links inside table cells render as plain text. Images inside a paragraph fall back to their alt text. Tables longer than 28 rows are split into stacked tables with the header repeated. Inline markup inside table cells renders as plain text. A document is capped at 1024 top-level blocks (paragraphs, headings, list items, ...); content past the cap is dropped with a warning.
- The web UI's Markdown converter (`/api/md2pdf`) supports a smaller dialect than `tspdf md2pdf`: headings, bullet lists, blockquotes, fenced code blocks, and rules. Tables, images, and inline bold/italic/code styling are CLI-only; in the web UI those markers render literally. The web endpoint deliberately embeds no images, since that would mean reading files on the server.
- Merge and split preserve bookmarks and form fields (split keeps only what points at kept pages; named destinations are flattened to explicit ones). Structure trees and page labels are still dropped.
- Merging files that use the same form field name leaves the duplicate names as-is; viewers will treat them as one shared field.
- Watermark support is text-only (no image watermark pipeline yet).
- PNG color-key transparency (a tRNS chunk on RGB or grayscale images, color types 0/2) is not mapped to an SMask: those pixels render opaque in the generated PDF. Palette tRNS (color type 3) is fully supported.
- Web server mode is intentionally simple and local-first; it is not a general-purpose multi-tenant service.

## PDF Coverage

- Real-world PDF diversity is large. While core manipulation paths are tested, unusual producer-specific edge cases can still surface.
- Xref-stream overflow guards are implemented; broader fixture coverage for all xref-stream edge variants is still evolving.

## Performance / Resource Behavior

- Very large or complex inputs may take noticeable time in single-process CLI/web mode.
- Decompression output is capped for safety; malformed or highly-expanding streams are rejected by design.
- Metadata JSON responses in web mode are bounded by request-size policy to avoid unbounded allocations.

## API / Integrator Expectations

- C library usage currently assumes source-tree style integration in many setups; packaged install ergonomics for headers/libs remain basic.
- Stable high-level CLI behavior is prioritized, but low-level internal APIs can still evolve.

## Current Non-Goals

- Cloud processing, telemetry, or remote file upload.
- Hidden network dependencies at runtime.

If you hit a limitation with a reproducible file/command, open an issue with:

1. command used,
2. expected behavior,
3. actual output/error,
4. sanitized sample input if possible.
