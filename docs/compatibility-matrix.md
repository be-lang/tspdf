# Compatibility Matrix

This matrix reflects what the project currently validates in CI and local regression tests.

## Platforms

| Platform | Status | Notes |
|---|---|---|
| Linux (x86_64) | Primary | Main development and CI target |
| macOS | Tested | CI runs the full suite with gcc and clang |
| Windows | Untested | Not supported today; the make build does not work natively |

## Input/Feature Coverage

| Area | Status | Coverage Source |
|---|---|---|
| Merge / Split / Rotate / Reorder / Delete | Stable | `tests/reader/`, `tests/test_cli.sh` |
| Encrypt / Decrypt (AES-128/256) | Stable | `tests/reader/`, `tests/test_crypto.c`, `tests/test_cli.sh` |
| Metadata view/edit | Stable | `tests/reader/`, `tests/test_cli.sh` |
| Watermark (text/image) | Stable | `tests/test_cli.sh` |
| Stamp / N-up / Crop / Scale | Stable | `tests/reader/`, `tests/test_cli.sh` |
| Forms (list/fill/flatten) | Stable | `tests/reader/`, `tests/test_cli.sh` |
| Attachments / Bookmarks | Stable | `tests/reader/`, `tests/test_cli.sh` |
| Text extraction | Stable | `tests/reader/`, `tests/test_cli.sh` |
| Compress (incl. `--lossy` image recompression) | Stable | `tests/unit/`, `tests/reader/`, `tests/test_cli.sh` |
| Images to PDF (img2pdf) / page numbers | Stable | `tests/test_cli.sh` |
| Markdown to PDF | Stable | `tests/test_cli.sh` |
| QR code to PDF | Stable | `tests/unit/`, `tests/test_cli.sh` |
| PNG decode | Hardened | `tests/unit/` |
| Deflate/inflate | Hardened | `tests/unit/` |
| Embedded web server | Hardened | `tests/test_cli.sh` raw HTTP regressions |

## Reliability Signals

- `make test-all` covers unit + reader + crypto + CLI/web flows.
- Raw HTTP regressions cover malformed headers, content-length handling, and stalled read timeout behavior.
- Security hardening includes size/overflow checks in xref parsing, PNG decode, and deflate output limits.

## How To Reproduce Locally

```bash
make test-all
```

For benchmark snapshots:

```bash
make bench
make bench-reader
```

See `.github/workflows/ci.yml` for CI execution and artifact collection details.
