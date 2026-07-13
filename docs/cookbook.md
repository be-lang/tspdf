# tspdf Cookbook

Copy-paste recipes for common tasks.

## Merge PDFs

```bash
tspdf merge a.pdf b.pdf c.pdf -o merged.pdf
```

## Split pages into a new file

```bash
tspdf split report.pdf --pages 1-5,9 -o extracted.pdf
```

## Split every page into its own file

```bash
# writes page-001.pdf, page-002.pdf, ...
tspdf split report.pdf -o page.pdf
```

Embedded file attachments are copied into every output. Add
`--no-attachments` to drop them instead:

```bash
tspdf split report.pdf --no-attachments -o page.pdf
```

## Stamp page numbers

```bash
tspdf pagenum report.pdf -o numbered.pdf

# "Page 1 of 12" at the top right, numbering starting at 10
tspdf pagenum report.pdf --format "Page %d of %d" --position top-right --start 10 -o numbered.pdf

# skip the cover page; page 2 still shows "2"
tspdf pagenum report.pdf --pages 2-12 -o numbered.pdf
```

## Delete pages

```bash
tspdf delete report.pdf --pages 2,4-6 -o cleaned.pdf
```

## Reorder pages

```bash
tspdf reorder report.pdf --order 3,1,2 -o reordered.pdf
```

## Rotate pages

```bash
tspdf rotate report.pdf --pages 1,2 --angle 90 -o rotated.pdf
```

## Crop pages (set the CropBox)

Cropping sets the visible region without deleting content, so it is safe and
reversible. The box is clamped to the MediaBox.

```bash
# explicit box, relative to the page's MediaBox origin (x0,y0,x1,y1)
tspdf crop report.pdf --box 50,50,545,742 -o cropped.pdf

# crop 1 inch (72 pt) off every side
tspdf crop report.pdf --margin 72 -o cropped.pdf

# per-side margins: top, right, bottom, left
tspdf crop report.pdf --margins 72,36,72,36 --pages 1-3 -o cropped.pdf
```

## Scale or resize pages

Scaling resizes the page and its content together (unlike cropping, which only
clips the view).

```bash
# fit each page to A4, aspect preserved and centered
tspdf scale report.pdf --to a4 -o a4.pdf

# uniform scale to 50%
tspdf scale report.pdf --factor 0.5 -o half.pdf
```

Named sizes for `--to`: `a4`, `letter`, `legal`, `a3`, `a5`.

## Add a text watermark

```bash
tspdf watermark report.pdf --text "DRAFT" -o draft.pdf
```

## Add an image watermark

```bash
# centered logo at 30% opacity
tspdf watermark report.pdf --image logo.png -o marked.pdf

# tiled across every page, smaller
tspdf watermark report.pdf --image logo.png --position tile --scale 0.2 -o marked.pdf
```

## Stamp one PDF onto another

```bash
# draw approved.pdf's first page over every page
tspdf stamp report.pdf --stamp approved.pdf -o stamped.pdf

# letterhead: put it under the existing content instead
tspdf stamp report.pdf --stamp letterhead.pdf --under -o out.pdf

# only page 1, using page 2 of the stamp file
tspdf stamp report.pdf --stamp marks.pdf --stamp-page 2 --pages 1 -o out.pdf

# encrypted input or stamp file: --password / --stamp-password, or read the
# password from a file so it stays out of the process list
tspdf stamp locked.pdf --stamp approved.pdf --password-file pw.txt -o out.pdf
tspdf stamp report.pdf --stamp locked-stamp.pdf --stamp-password-file pw.txt -o out.pdf
```

## Place multiple pages per sheet (N-up)

```bash
# 2 slides per page for a handout
tspdf nup 2 slides.pdf -o handout.pdf

# 4-up with a border around each page and a 12pt gap, on letter paper
tspdf nup 4 report.pdf --frame --gap 12 --page-size letter -o report-4up.pdf

# 6-up landscape of a page range
tspdf nup 6 deck.pdf --pages 1-12 --landscape -o deck-6up.pdf
```

N is one of 2, 4, 6, 8, 9, 16. Each page is scaled to fit its cell, keeping
its aspect ratio, in reading order. Bookmarks, forms, and annotations are not
carried over.

## Extract text

```bash
# all pages to stdout, one form-feed between pages
tspdf text report.pdf

# specific pages, to a file
tspdf text report.pdf --pages 1-3 -o report.txt

# keep the page layout: columns and tables stay aligned (like pdftotext -layout)
tspdf text report.pdf --layout -o report.txt
```

## Compress a PDF

```bash
tspdf compress report.pdf -o smaller.pdf

# scans and photo-heavy files: also downsample images and re-encode as JPEG
# (photos) or CCITT G4 (black-and-white scans)
tspdf compress --lossy scan.pdf -o small.pdf

# tune it (defaults: photos 150 dpi quality 75, black-and-white 300 dpi)
tspdf compress --lossy --image-dpi 100 --image-quality 60 scan.pdf -o tiny.pdf
tspdf compress --lossy --mono-dpi 200 book-scan.pdf -o small-book.pdf
```

`--lossy` reduces image quality. Images with transparency or unusual color
spaces are left alone, as is anything within 1.3x of the target dpi (too
close to be worth re-encoding). Black-and-white images get their own
`--mono-dpi` target because text needs more resolution than photos to stay
readable.

## Fill form fields

```bash
# see what the form has
tspdf form list application.pdf

# fill fields (repeatable --set, or a JSON file / stdin via --data)
tspdf form fill application.pdf \
  --set name="Ada Lovelace" \
  --set agree=true \
  -o filled.pdf

# stamp the values into the page and remove the form
tspdf form flatten filled.pdf -o final.pdf
```

Values with characters outside WinAnsi (CJK, Greek, ...) need a fallback
font for display. tspdf looks for one automatically in the system font
directories (TrueType only; `TSPDF_FONT_DIRS=/dir:/dir` replaces the scan
roots); to pick one yourself:

```bash
TSPDF_FALLBACK_FONT=/usr/share/fonts/some/font.ttf \
  tspdf form fill application.pdf --set name=日本語 -o filled.pdf
```

If no usable font is found the value is still stored, but displays as `?`
(tspdf prints a warning).

## Read or update metadata

```bash
# view
tspdf metadata report.pdf

# update
tspdf metadata report.pdf \
  --set title="Quarterly Report" \
  --set author="Finance Team" \
  -o report-updated.pdf
```

This edits the Info dictionary. If the document also carries XMP metadata,
the matching XMP properties are updated as well, because some viewers
(Acrobat among them) show the XMP values instead. Fields whose property is
missing from the XMP packet are left alone there (tspdf never invents XMP
structure) and a notice names them.

## Edit bookmarks (outline)

```bash
# list existing bookmarks (LEVEL<TAB>PAGE<TAB>TITLE, one per line)
tspdf bookmark list report.pdf

# machine-readable
tspdf bookmark list report.pdf --json

# import a table of contents from a file (or - for stdin).
# each line: LEVEL <TAB> PAGE <TAB> TITLE
#   LEVEL is 1-based nesting (1 = top; no jumps > 1)
#   PAGE  is 1-based
printf '1\t1\tIntroduction\n2\t2\tBackground\n1\t5\tResults\n' > toc.txt
tspdf bookmark import report.pdf --from toc.txt -o outlined.pdf

# add the imported entries after the existing outline instead of replacing it
tspdf bookmark import report.pdf --from extra.txt --append -o outlined.pdf

# append a single bookmark
tspdf bookmark add report.pdf --title "Appendix" --page 12 --level 1 -o outlined.pdf

# remove all bookmarks
tspdf bookmark clear report.pdf -o plain.pdf
```

`bookmark add` and `bookmark import --append` leave the existing entries
untouched: destinations (scroll position, zoom, view type), link actions,
colors, styles, and collapsed state all carry over.

`bookmark list` prints the exact format `bookmark import` reads, so you can
list, edit, and re-import:

```bash
tspdf bookmark list report.pdf > toc.txt
$EDITOR toc.txt
tspdf bookmark import report.pdf --from toc.txt -o report-new.pdf
```

## Embed file attachments

```bash
# embed files (stored under their base names, with size, modification date,
# checksum, and a MIME type from the extension)
tspdf attach add report.pdf data.csv notes.txt -o bundled.pdf

# set the MIME type yourself
tspdf attach add report.pdf export.dat --mime application/x-custom -o bundled.pdf

# list / extract / remove
tspdf attach list bundled.pdf --json
tspdf attach extract bundled.pdf --all -o out-dir/
tspdf attach remove bundled.pdf --name notes.txt -o trimmed.pdf
```

## Encrypt and decrypt

```bash
# AES-128 (default)
tspdf encrypt report.pdf --password "secret" -o locked.pdf

# AES-256
tspdf encrypt report.pdf --password "secret" --bits 256 -o locked-256.pdf

# decrypt
tspdf decrypt locked.pdf --password "secret" -o unlocked.pdf
```

Every command that reads a PDF takes `--password` (or `--password-file`,
which reads the first line of a file — `-` for stdin — so the password stays
out of the process list). The output keeps the original encryption and
passwords; use `decrypt` to remove them:

```bash
tspdf rotate locked.pdf --password "secret" --angle 90 -o rotated.pdf   # still encrypted
tspdf split locked.pdf --password-file pw.txt --pages 1-3 -o part.pdf
```

One exception: `tspdf merge` unlocks encrypted inputs (the one password is
tried on every input) but always writes an unencrypted document, since
several sources have no single encryption to carry over; re-encrypt the
result if it must stay protected.

## Convert images to PDF

```bash
tspdf img2pdf page1.jpg page2.png -o photos.pdf

# page sized to each image instead of A4
tspdf img2pdf scan1.png scan2.png --page-size image -o scans.pdf
```

## Generate a QR code PDF

```bash
tspdf qrcode "https://example.com" --title "Scan me" -o qr.pdf
```

## Convert Markdown to PDF

```bash
# supports headings, lists, code blocks, pipe tables and ![alt](img.png) images
# headings become PDF bookmarks; [text](url) links stay clickable
tspdf md2pdf notes.md -o notes.pdf
```

## Run local web UI

```bash
tspdf serve
# then open http://localhost:8080
```

Use a custom port:

```bash
tspdf serve --port 3000
```

Run it in Docker (binds 0.0.0.0 inside the container; the UI has no authentication):

```bash
docker build -t tspdf . && docker run -p 127.0.0.1:8080:8080 tspdf
```

## Troubleshooting quick checks

```bash
# show command-specific help
tspdf help merge

# print version
tspdf --version
```
