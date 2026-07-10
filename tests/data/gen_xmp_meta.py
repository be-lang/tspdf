# Regenerate xmp_meta.pdf: a 1-page PDF that carries an XMP metadata stream
# (catalog /Metadata) with a dc:title, plus a matching Info /Title. Used to
# test the "XMP metadata present and not updated" notice of tspdf metadata.
# Run with:  uv run --python 3.12 --with pikepdf python tests/data/gen_xmp_meta.py
import pathlib

import pikepdf

pdf = pikepdf.new()
pdf.add_blank_page(page_size=(200, 200))

xmp = b"""<?xpacket begin="\xef\xbb\xbf" id="W5M0MpCehiHzreSzNTczkc9d"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about="" xmlns:dc="http://purl.org/dc/elements/1.1/">
   <dc:title><rdf:Alt><rdf:li xml:lang="x-default">Old XMP title</rdf:li></rdf:Alt></dc:title>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>
<?xpacket end="w"?>"""

stream = pikepdf.Stream(pdf, xmp)
stream.Type = pikepdf.Name.Metadata
stream.Subtype = pikepdf.Name.XML
pdf.Root.Metadata = pdf.make_indirect(stream)
with pdf.open_metadata(update_docinfo=False) as _:
    pass  # validates the XMP parses
pdf.docinfo["/Title"] = "Old XMP title"

out = pathlib.Path(__file__).with_name("xmp_meta.pdf")
pdf.save(out)
print("wrote", out)
