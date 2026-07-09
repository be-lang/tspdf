# Known Limitations

This file tracks current, known constraints so users can evaluate fit quickly.

See the [compatibility matrix](compatibility-matrix.md) for what is tested where.

## Scope

- Text extraction (`tspdf text`) reads text in content-stream order: no column re-ordering, no OCR of scanned pages, and no blank-line preservation. CID fonts without a /ToUnicode map extract as U+FFFD (a stderr warning names the affected pages).
- No rendering of pages to images.
- Existing AcroForm forms cannot be filled (creating form fields in generated PDFs is supported).
- md2pdf does not render tables or images.
- Merge and split currently drop bookmarks and form fields.
- Watermark support is text-only (no image watermark pipeline yet).
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
