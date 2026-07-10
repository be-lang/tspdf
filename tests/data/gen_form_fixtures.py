# Regenerates the AcroForm fixtures used by the form tests:
#
#   form_fields.pdf     one page: text field "name" (value "Ada"), checkbox
#                       "agree" (off, on-state /Yes), radio group "color"
#                       (/Red /Blue, value /Red), combobox "city", and a
#                       nested text field "a.b" (parent /T (a), kid /T (b)).
#   form_fields_enc.pdf the same document encrypted (AES-128, user pw "secret")
#
# Run from the repo root:
#   uv run --python 3.12 --with pymupdf==1.23.26 python tests/data/gen_form_fixtures.py
#
# pymupdf 1.23.x cannot create radio groups or hierarchical fields through
# the Widget API, so those two are assembled with the low-level xref calls.

import fitz

W, H = 612, 792


def add_text(page, name, rect, value=""):
    w = fitz.Widget()
    w.field_name = name
    w.field_type = fitz.PDF_WIDGET_TYPE_TEXT
    w.rect = fitz.Rect(*rect)
    w.field_value = value
    page.add_widget(w)


def add_checkbox(page, name, rect, checked=False):
    w = fitz.Widget()
    w.field_name = name
    w.field_type = fitz.PDF_WIDGET_TYPE_CHECKBOX
    w.rect = fitz.Rect(*rect)
    w.field_value = checked
    page.add_widget(w)


def add_combobox(page, name, rect, options, value):
    w = fitz.Widget()
    w.field_name = name
    w.field_type = fitz.PDF_WIDGET_TYPE_COMBOBOX
    w.rect = fitz.Rect(*rect)
    w.choice_values = options
    w.field_value = value
    page.add_widget(w)


def append_field(doc, field_xref):
    # /AcroForm may be an indirect object or inline in the catalog.
    cat = doc.pdf_catalog()
    t, v = doc.xref_get_key(cat, "AcroForm")
    if t == "xref":
        obj, key = int(v.split()[0]), "Fields"
    else:
        obj, key = cat, "AcroForm/Fields"
    t, fields = doc.xref_get_key(obj, key)
    assert t == "array", (t, fields)
    doc.xref_set_key(obj, key, fields[:-1] + f" {field_xref} 0 R]")


def widget_ap(doc, xref, on_state):
    # Minimal down/off appearance streams so the widget has real /AP /N keys.
    on = doc.get_new_xref()
    doc.update_object(on, "<< /Type /XObject /Subtype /Form /BBox [0 0 15 15] >>")
    doc.update_stream(on, b"q 0 g 3 3 m 12 12 l 3 12 m 12 3 l S Q")
    off = doc.get_new_xref()
    doc.update_object(off, "<< /Type /XObject /Subtype /Form /BBox [0 0 15 15] >>")
    doc.update_stream(off, b"")
    doc.xref_set_key(xref, "AP", f"<< /N << /{on_state} {on} 0 R /Off {off} 0 R >> >>")


def add_radio_group(doc, page, name, states, rects, value):
    parent = doc.get_new_xref()
    kid_xrefs = []
    for state, rect in zip(states, rects):
        kid = doc.get_new_xref()
        r = fitz.Rect(*rect)
        as_name = state if state == value else "Off"
        doc.update_object(
            kid,
            f"<< /Type /Annot /Subtype /Widget /F 4 "
            f"/Rect [{r.x0} {r.y0} {r.x1} {r.y1}] "
            f"/P {page.xref} 0 R /Parent {parent} 0 R /AS /{as_name} /MK << /CA (l) >> >>",
        )
        widget_ap(doc, kid, state)
        kid_xrefs.append(kid)
    kids = " ".join(f"{k} 0 R" for k in kid_xrefs)
    doc.update_object(
        parent,
        f"<< /FT /Btn /T ({name}) /Ff {1 << 15} /V /{value} /Kids [{kids}] >>",
    )
    append_field(doc, parent)
    for k in kid_xrefs:
        annots_t, annots = doc.xref_get_key(page.xref, "Annots")
        assert annots_t == "array"
        doc.xref_set_key(page.xref, "Annots", annots[:-1] + f" {k} 0 R]")


def add_nested_text(doc, page, parent_name, kid_name, rect):
    parent = doc.get_new_xref()
    kid = doc.get_new_xref()
    r = fitz.Rect(*rect)
    doc.update_object(
        kid,
        f"<< /Type /Annot /Subtype /Widget /F 4 /T ({kid_name}) "
        f"/Rect [{r.x0} {r.y0} {r.x1} {r.y1}] "
        f"/P {page.xref} 0 R /Parent {parent} 0 R >>",
    )
    doc.update_object(parent, f"<< /FT /Tx /T ({parent_name}) /Kids [{kid} 0 R] >>")
    append_field(doc, parent)
    annots_t, annots = doc.xref_get_key(page.xref, "Annots")
    assert annots_t == "array"
    doc.xref_set_key(page.xref, "Annots", annots[:-1] + f" {kid} 0 R]")


def build():
    doc = fitz.open()
    page = doc.new_page(width=W, height=H)
    page.insert_text((72, 80), "tspdf form fixture", fontsize=14)

    add_text(page, "name", (72, 100, 300, 120), "Ada")
    add_checkbox(page, "agree", (72, 140, 87, 155), checked=False)
    add_combobox(page, "city", (72, 180, 220, 200), ["Berlin", "Paris", "Oslo"], "Berlin")
    add_radio_group(doc, page, "color", ["Red", "Blue"], [(72, 220, 87, 235), (100, 220, 115, 235)], "Red")
    add_nested_text(doc, page, "a", "b", (72, 260, 300, 280))

    doc.save("tests/data/form_fields.pdf")
    doc.save(
        "tests/data/form_fields_enc.pdf",
        encryption=fitz.PDF_ENCRYPT_AES_128,
        user_pw="secret",
        owner_pw="secret",
    )
    doc.close()
    print("wrote tests/data/form_fields.pdf and form_fields_enc.pdf")


if __name__ == "__main__":
    build()
