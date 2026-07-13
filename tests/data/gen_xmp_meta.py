# Regenerate the XMP metadata fixtures:
#   xmp_meta.pdf       title-only packet; a --set of any other field cannot be
#                      applied to XMP, so the CLI must print the stale notice
#                      for that field.
#   xmp_meta_full.pdf  packet with dc:title/creator/description, pdf:Keywords,
#                      pdf:Producer, xmp:CreatorTool and xmp:ModifyDate plus
#                      real xpacket padding; every metadata --set key syncs.
#   xmp_meta_flate.pdf same packet but Flate-compressed (/Filter FlateDecode).
# Each also carries a matching Info /Title.
# Run with:  uv run --python 3.12 --with pikepdf python tests/data/gen_xmp_meta.py
import pathlib
import zlib

import pikepdf

TITLE_ONLY = b"""<?xpacket begin="\xef\xbb\xbf" id="W5M0MpCehiHzreSzNTczkc9d"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about="" xmlns:dc="http://purl.org/dc/elements/1.1/">
   <dc:title><rdf:Alt><rdf:li xml:lang="x-default">Old XMP title</rdf:li></rdf:Alt></dc:title>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>
<?xpacket end="w"?>"""

FULL = (
    b"""<?xpacket begin="\xef\xbb\xbf" id="W5M0MpCehiHzreSzNTczkc9d"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about=""
    xmlns:dc="http://purl.org/dc/elements/1.1/"
    xmlns:pdf="http://ns.adobe.com/pdf/1.3/"
    xmlns:xmp="http://ns.adobe.com/xap/1.0/">
   <dc:title><rdf:Alt><rdf:li xml:lang="x-default">Old XMP title</rdf:li></rdf:Alt></dc:title>
   <dc:creator><rdf:Seq><rdf:li>Old XMP author</rdf:li></rdf:Seq></dc:creator>
   <dc:description><rdf:Alt><rdf:li xml:lang="x-default">Old XMP subject</rdf:li></rdf:Alt></dc:description>
   <pdf:Keywords>old, keywords</pdf:Keywords>
   <pdf:Producer>Old XMP producer</pdf:Producer>
   <xmp:CreatorTool>Old XMP creator tool</xmp:CreatorTool>
   <xmp:ModifyDate>2001-01-01T00:00:00Z</xmp:ModifyDate>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>
"""
    + (b" " * 63 + b"\n") * 32  # xpacket padding
    + b"""<?xpacket end="w"?>"""
)


def build(xmp, out_name, flate=False):
    pdf = pikepdf.new()
    pdf.add_blank_page(page_size=(200, 200))
    stream = pikepdf.Stream(pdf, xmp)
    if flate:
        stream.write(zlib.compress(xmp), filter=pikepdf.Name.FlateDecode)
    stream.Type = pikepdf.Name.Metadata
    stream.Subtype = pikepdf.Name.XML
    pdf.Root.Metadata = pdf.make_indirect(stream)
    with pdf.open_metadata(update_docinfo=False) as _:
        pass  # validates the XMP parses
    pdf.docinfo["/Title"] = "Old XMP title"
    out = pathlib.Path(__file__).with_name(out_name)
    pdf.save(out)
    print("wrote", out)


build(TITLE_ONLY, "xmp_meta.pdf")
build(FULL, "xmp_meta_full.pdf")
build(FULL, "xmp_meta_flate.pdf", flate=True)
