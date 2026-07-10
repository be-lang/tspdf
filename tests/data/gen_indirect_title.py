# Regenerate indirect_title.pdf: a 2-page PDF whose outline stores /Title
# (and one /Dest) as indirect objects, the way pdfTeX/hyperref emits them.
# Run with:  uv run --python 3.12 --with pikepdf python tests/data/gen_indirect_title.py
import pikepdf
from pikepdf import Dictionary, Name, Array, String

pdf = pikepdf.new()
pdf.add_blank_page(page_size=(200, 200))
pdf.add_blank_page(page_size=(200, 200))

outlines = pdf.make_indirect(Dictionary(Type=Name.Outlines))
ch1 = pdf.make_indirect(Dictionary())
sub = pdf.make_indirect(Dictionary())
ch2 = pdf.make_indirect(Dictionary())

# Indirect /Title strings (the pdfTeX pattern under test).
ch1.Title = pdf.make_indirect(String("Alpha"))
sub.Title = pdf.make_indirect(String("Alpha Sub"))
ch2.Title = pdf.make_indirect(String("Beta"))

ch1.Parent = outlines
ch1.Dest = Array([pdf.pages[0].obj, Name.Fit])
ch1.First = sub
ch1.Last = sub
ch1.Count = 1
ch1.Next = ch2

sub.Parent = ch1
# Indirect /Dest array for coverage of indirect non-string values.
sub.Dest = pdf.make_indirect(Array([pdf.pages[1].obj, Name.Fit]))

ch2.Parent = outlines
ch2.Prev = ch1
ch2.Dest = Array([pdf.pages[1].obj, Name.Fit])

outlines.First = ch1
outlines.Last = ch2
outlines.Count = 3

pdf.Root.Outlines = outlines
pdf.save("tests/data/indirect_title.pdf", static_id=True,
         object_stream_mode=pikepdf.ObjectStreamMode.disable,
         compress_streams=False)
