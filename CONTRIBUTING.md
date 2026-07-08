# Contributing to tspdf

Thanks for your interest in tspdf. This is a zero-dependency C11 PDF toolkit,
and contributions that keep it correct, safe, and dependency-free are very
welcome.

## Ground rules

- **No new dependencies.** tspdf builds with only a C11 compiler and `make`,
  not even zlib. Everything (deflate, crypto, PNG, TrueType, layout) is
  implemented from scratch. Please keep it that way.
- **The reader parses untrusted input.** Any change under `src/reader/`,
  `src/compress/`, `src/image/`, or `src/font/` must assume hostile input:
  validate lengths before pointer arithmetic, guard size arithmetic against
  overflow, and never trust a file-declared size to drive an allocation.
- **Match the surrounding style.** No formatter is enforced; follow the
  conventions of the file you are editing.

## Building and testing

```bash
make                 # build the CLI (build/tspdf) and library
make test-all        # unit + reader + crypto + CLI/web regression tests
make check           # clean -Werror rebuild + full suite (the CI gate)
```

Before opening a pull request, make sure the sanitizer and conformance gates
pass for anything you touched:

```bash
make test-asan          # writer/layout/font/image/compress + crypto under ASan+UBSan+Leak
make test-asan-reader   # reader/parser suite under the same sanitizers
make test-external      # round-trip output through qpdf --check / mutool (needs qpdf, mupdf-tools)
make fuzz               # build the libFuzzer harnesses (needs clang)
```

If you change any parser, run the relevant fuzzer for a minute or two and add
any crash reproducer you find to `fuzz/corpus/<harness>/` as a regression seed.

## Test-driven changes

tspdf is developed test-first. For a bug fix or feature:

1. Add a failing test (`tests/test_reader.c` for reader/parser work,
   `tests/test_main.c` / `tests/test_crypto.c` for the writer and crypto,
   `tests/test_cli.sh` for CLI and web behavior).
2. Run it and watch it fail.
3. Make the minimal change that turns it green.
4. Keep the whole suite green.

## Pull requests

- Keep each PR focused on one change; small, reviewable commits are preferred.
- Describe what the change does and how you verified it. If it fixes a PDF that
  wouldn't open, attach or link the file when you can.
- Confirm `make check` passes locally. CI runs the same gate across
  `{Linux, macOS} × {gcc, clang}`, a sanitizer job, and the conformance gate.
- Update `CHANGELOG.md` under `[Unreleased]` for user-visible changes.

## Reporting bugs and PDFs that won't open

Broad real-world PDF compatibility is a primary goal. If tspdf fails on a file,
please open an issue with the file (or a minimal reproducer) attached. Those
reports are the most valuable kind. For security issues, see
[SECURITY.md](SECURITY.md) instead of the public tracker.

## License

By contributing, you agree that your contributions are licensed under the
project's [MIT License](LICENSE).
