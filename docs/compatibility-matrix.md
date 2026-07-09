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
| Merge / Split / Rotate / Reorder / Delete | Stable | `tests/test_reader.c`, `tests/test_cli.sh` |
| Encrypt / Decrypt (AES-128/256) | Stable | `tests/test_reader.c`, `tests/test_crypto.c`, `tests/test_cli.sh` |
| Metadata view/edit | Stable | `tests/test_reader.c`, `tests/test_cli.sh` |
| Watermark (text) | Stable | `tests/test_cli.sh` |
| Text extraction | Stable | `tests/test_reader.c`, `tests/test_cli.sh` |
| Compress | Stable | `tests/test_main.c`, `tests/test_cli.sh` |
| Markdown to PDF | Stable | `tests/test_cli.sh` |
| QR code to PDF | Stable | `tests/test_main.c`, `tests/test_cli.sh` |
| PNG decode | Hardened | `tests/test_main.c` |
| Deflate/inflate | Hardened | `tests/test_main.c` |
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
