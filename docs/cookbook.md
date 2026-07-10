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

## Add a text watermark

```bash
tspdf watermark report.pdf --text "DRAFT" -o draft.pdf
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

## Encrypt and decrypt

```bash
# AES-128 (default)
tspdf encrypt report.pdf --password "secret" -o locked.pdf

# AES-256
tspdf encrypt report.pdf --password "secret" --bits 256 -o locked-256.pdf

# decrypt
tspdf decrypt locked.pdf --password "secret" -o unlocked.pdf
```

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
