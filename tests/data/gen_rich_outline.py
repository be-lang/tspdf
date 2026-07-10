# Regenerate rich_outline.pdf: a 3-page PDF with a rich outline that the
# bookmark commands must preserve on edit: XYZ destinations with scroll
# coordinates and zoom, a collapsed chapter (negative /Count), and a colored
# bold+italic entry (/C and /F).
# Run with:  uv run --python 3.12 --with pymupdf python tests/data/gen_rich_outline.py
import pathlib

import pymupdf

doc = pymupdf.open()
for _ in range(3):
    doc.new_page(width=612, height=792)

toc = [
    [1, "Chapter 1", 1, {"kind": pymupdf.LINK_GOTO,
                         "to": pymupdf.Point(72, 92), "zoom": 1.5,
                         "color": (1, 0, 0), "bold": True, "italic": True}],
    [2, "Section 1.1", 2, {"kind": pymupdf.LINK_GOTO,
                           "to": pymupdf.Point(36, 292), "zoom": 0}],
    [1, "Chapter 2", 3, {"kind": pymupdf.LINK_GOTO,
                         "to": pymupdf.Point(0, 0), "zoom": 2}],
]
# collapse=1: everything below level 1 starts collapsed, so Chapter 1 is
# stored with a negative /Count.
doc.set_toc(toc, collapse=1)

out = pathlib.Path(__file__).with_name("rich_outline.pdf")
doc.save(out, deflate=True)
print("wrote", out)
