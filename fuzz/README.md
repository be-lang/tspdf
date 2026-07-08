# Fuzzing harnesses

Coverage-guided fuzz targets over tspdf's from-scratch parsers — the code paths
that consume untrusted input. They are plain C translation units compiled with
clang's built-in libFuzzer + sanitizers, so the zero-third-party-dependency
promise holds: no vendored fuzzing library, just the compiler.

## Targets

| Harness            | Entry point under test          | Surface |
|--------------------|---------------------------------|---------|
| `fuzz_reader.c`    | `tspdf_reader_open` + page walk + `save_to_memory` | whole PDF parsing, page tree, re-serialization |
| `fuzz_inflate.c`   | `deflate_decompress`            | DEFLATE/zlib inflate (FlateDecode, PNG IDAT) |
| `fuzz_ttf.c`       | `ttf_load_from_memory` + lookups | TrueType/OpenType table parsing |
| `fuzz_png.c`       | `png_image_load_mem`            | PNG chunk parsing + unfiltering |

## Build & run

```sh
make fuzz                       # builds build/fuzz/* with clang -fsanitize=fuzzer,address,undefined
./build/fuzz/fuzz_reader  fuzz/corpus/reader
./build/fuzz/fuzz_inflate fuzz/corpus/inflate
./build/fuzz/fuzz_ttf     fuzz/corpus/ttf
./build/fuzz/fuzz_png     fuzz/corpus/png
```

Useful libFuzzer flags: `-max_total_time=60` (stop after 60s),
`-max_len=65536`, `-runs=1000000`. A reproducer for a crash is written to the
current directory as `crash-<sha1>`; replay it with
`./build/fuzz/fuzz_<name> ./crash-<sha1>`.

Override the compiler for AFL: `make fuzz FUZZ_CC=afl-clang-fast`.

## Seed corpus

`fuzz/corpus/<target>/` holds small, structurally-valid starting inputs so the
fuzzer begins inside the parser instead of at byte zero:

- `inflate/hello.zlib` — a real zlib stream.
- `ttf/minimal.ttf` — a minimal sfnt offset table + one directory entry.
- `png/sample.png` — a valid PNG (copied from `examples/test.png`).
- `reader/` — populated by `make fuzz-corpus` from `tests/data/*.pdf`
  (those copies are gitignored and regenerated on demand).

Coverage-increasing units and crash reproducers that the fuzzer writes at run
time are gitignored (see `fuzz/.gitignore`); only the curated seeds are tracked.
