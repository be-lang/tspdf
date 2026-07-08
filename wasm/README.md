# wasm

The tspdf reader tools compiled to WebAssembly, plus a static in-browser
demo of the web UI. Everything runs client-side; no server, no uploads.

Requires emcc (Emscripten). These targets are optional and not part of
`make test-all`.

    make wasm        # build wasm/dist/tspdf.{js,wasm} (ES6 module)
    make wasm-test   # run all operations under node, qpdf --check outputs
    make wasm-demo   # assemble the static demo site into wasm/demo/dist/

Files:

- `shim.c` — C ABI exported to JavaScript (open/merge/extract/rotate/…)
- `tspdf-api.js` — JS wrapper over the ABI, shared by node tests and browser
- `demo/wasm-backend.js` — implements the web UI's backend in the browser
- `demo/build_demo.py` — renders the served templates into a static site
- `test/` — node harness + qpdf gate for `make wasm-test`

The demo deploys to GitHub Pages via `.github/workflows/pages.yml`
(Pages must be enabled in the repo settings: Source "GitHub Actions").

Tools covered: merge, split, delete pages, rotate, reorder, compress,
unlock, password protect, metadata, watermark. The writer-side tools
(img2pdf, md2pdf, qrcode) are not in the wasm build.
