#!/usr/bin/env python3
"""Assemble the static in-browser demo into wasm/demo/dist/.

Renders the same templates 'tspdf serve' embeds (web/templates/) with the
same block-substitution semantics as the C server, rewrites the absolute
server URLs into relative static-site paths (so the site works under a
GitHub Pages project subpath), drops the tools the wasm build does not
support (img2pdf, md2pdf, qrcode — writer-side), and injects the wasm
backend module. Build-time tooling only; stdlib only.

Run from the repo root (make wasm-demo does): python3 wasm/demo/build_demo.py
"""

import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TEMPLATES = ROOT / "web" / "templates"
STATIC = ROOT / "web" / "static"
WASM_DIST = ROOT / "wasm" / "dist"
DEMO_DIR = ROOT / "wasm" / "demo"
DIST = DEMO_DIR / "dist"

# Reader-side tools the wasm backend implements (see wasm-backend.js).
SUPPORTED_TOOLS = [
    "merge", "split", "delete-pages", "rotate", "reorder",
    "compress", "unlock", "password-protect", "metadata",
    "watermark-existing",
]
UNSUPPORTED_TOOLS = ["img2pdf", "md2pdf", "qrcode"]

BLOCK_RE = re.compile(r"\{%\s*block\s+(\w+)\s*%\}(.*?)\{%\s*endblock\s*%\}", re.S)


def render(child_text: str) -> str:
    """Substitute child blocks into base.html (mirrors cli/server.c)."""
    base = (TEMPLATES / "base.html").read_text()
    child_blocks = {m.group(1): m.group(2) for m in BLOCK_RE.finditer(child_text)}

    def repl(m):
        return child_blocks.get(m.group(1), m.group(2))

    return BLOCK_RE.sub(repl, base)


def rewrite_urls(html: str, prefix: str) -> str:
    """Turn the server's absolute URLs into relative static-site paths.

    prefix is '' for index.html and '../' for pages in tool/.
    """
    html = html.replace('href="/static/', f'href="{prefix}static/')
    html = html.replace('src="/static/', f'src="{prefix}static/')
    html = re.sub(r'href="/tool/([a-z0-9-]+)"',
                  lambda m: f'href="{prefix}tool/{m.group(1)}.html"', html)
    html = html.replace('href="/"', f'href="{prefix}index.html"')
    return html


def inject_backend(html: str, prefix: str) -> str:
    tag = f'<script type="module" src="{prefix}wasm-backend.js"></script>\n</body>'
    return html.replace("</body>", tag, 1)


def build():
    if not (WASM_DIST / "tspdf.js").exists():
        sys.exit("wasm/dist/tspdf.js not found — run 'make wasm' first")

    if DIST.exists():
        shutil.rmtree(DIST)
    (DIST / "tool").mkdir(parents=True)
    (DIST / "static").mkdir()

    # Index: drop the writer-side tool cards and the then-empty Create tab.
    index = (TEMPLATES / "index.html").read_text()
    for tool in UNSUPPORTED_TOOLS:
        index = re.sub(
            r'\s*<a href="/tool/' + re.escape(tool) + r'".*?</a>', "", index, flags=re.S)
    index = re.sub(r'\s*<button class="filter-tab" data-filter="create">Create</button>',
                   "", index)
    html = inject_backend(rewrite_urls(render(index), ""), "")
    html = html.replace(
        "Ready. Files never leave this device.",
        "Ready. Runs fully in your browser via WebAssembly — "
        "files never leave this device.")
    (DIST / "index.html").write_text(html)

    # Tool pages.
    for tool in SUPPORTED_TOOLS:
        src = TEMPLATES / "tools" / f"{tool}.html"
        html = inject_backend(rewrite_urls(render(src.read_text()), "../"), "../")
        (DIST / "tool" / f"{tool}.html").write_text(html)

    # Assets: served UI files + wasm module + backend glue.
    shutil.copy(STATIC / "style.css", DIST / "static")
    shutil.copy(STATIC / "app.js", DIST / "static")
    shutil.copy(WASM_DIST / "tspdf.js", DIST)
    shutil.copy(WASM_DIST / "tspdf.wasm", DIST)
    shutil.copy(ROOT / "wasm" / "tspdf-api.js", DIST)
    shutil.copy(DEMO_DIR / "wasm-backend.js", DIST)
    (DIST / ".nojekyll").write_text("")

    n_files = sum(1 for p in DIST.rglob("*") if p.is_file())
    print(f"demo site: {DIST} ({n_files} files, "
          f"{len(SUPPORTED_TOOLS)} tools)")


if __name__ == "__main__":
    build()
