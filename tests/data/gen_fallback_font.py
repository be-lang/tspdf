# Regenerates the synthetic fallback-font fixtures used by the form
# fallback-font tests:
#
#   fallback_font.ttf   TrueType font with real (box) outlines for basic
#                       Latin (space, ?, A-Z, a-z, 0-9) plus the CJK glyphs
#                       needed by the tests: 日本語テスト.
#   fallback_font.ttc   the same font wrapped in a single-face TrueType
#                       collection (exercises the ttc header path).
#   fallback_latin.ttf  the same font WITHOUT the CJK glyphs (a candidate
#                       that must be rejected by the coverage check).
#
# The outlines are generated programmatically (unique filled boxes per glyph,
# so rendered output visibly differs from a '?' fallback), which keeps the
# fixtures small and free of any third-party font license.
#
# Run from the repo root:
#   uv run --python 3.12 --with fonttools python tests/data/gen_fallback_font.py

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTCollection, TTFont

UPM = 1000
LATIN = " ?" + "".join(chr(c) for c in range(ord("A"), ord("Z") + 1)) \
             + "".join(chr(c) for c in range(ord("a"), ord("z") + 1)) \
             + "0123456789"
CJK = "日本語テスト"


def draw_glyph(index):
    # A distinct, clearly visible outline per glyph: a tall filled box whose
    # inner cut-out shifts with the glyph index. Enough variation that any
    # two strings render differently, which is what the pixel tests compare.
    pen = TTGlyphPen(None)
    pen.moveTo((60, 0))
    pen.lineTo((60, 700))
    pen.lineTo((540, 700))
    pen.lineTo((540, 0))
    pen.closePath()
    x = 100 + (index * 37) % 300
    y = 80 + (index * 53) % 400
    pen.moveTo((x, y))
    pen.lineTo((x + 120, y))
    pen.lineTo((x + 120, y + 160))
    pen.lineTo((x, y + 160))
    pen.closePath()
    return pen.glyph()


def build_font(chars, family):
    names = [".notdef"] + [f"g{ord(c):04X}" for c in chars]
    fb = FontBuilder(UPM, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap({ord(c): f"g{ord(c):04X}" for c in chars})
    glyphs = {".notdef": draw_glyph(0)}
    for i, c in enumerate(chars):
        glyphs[f"g{ord(c):04X}"] = draw_glyph(i + 1)
    fb.setupGlyf(glyphs)
    metrics = {name: (600, 60) for name in names}
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({"familyName": family, "styleName": "Regular",
                       "psName": family.replace(" ", "")})
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800,
                usWinDescent=200)
    fb.setupPost()
    return fb.font


def main():
    full = build_font(LATIN + CJK, "TspdfTestFallback")
    full.save("tests/data/fallback_font.ttf")

    ttc = TTCollection()
    ttc.fonts = [TTFont("tests/data/fallback_font.ttf")]
    ttc.save("tests/data/fallback_font.ttc")

    latin = build_font(LATIN, "TspdfTestLatin")
    latin.save("tests/data/fallback_latin.ttf")
    print("wrote tests/data/fallback_font.ttf, fallback_font.ttc, "
          "fallback_latin.ttf")


if __name__ == "__main__":
    main()
