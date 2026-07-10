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
```

## Compress a PDF

```bash
tspdf compress report.pdf -o smaller.pdf
```

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

# append a single bookmark
tspdf bookmark add report.pdf --title "Appendix" --page 12 --level 1 -o outlined.pdf

# remove all bookmarks
tspdf bookmark clear report.pdf -o plain.pdf
```

`bookmark list` prints the exact format `bookmark import` reads, so you can
list, edit, and re-import:

```bash
tspdf bookmark list report.pdf > toc.txt
$EDITOR toc.txt
tspdf bookmark import report.pdf --from toc.txt -o report-new.pdf
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

Editing an encrypted PDF (`form fill`, `bookmark add`, `stamp`, ...) with
`--password` keeps the output encrypted with the same passwords. Use
`decrypt` to remove the password.

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
