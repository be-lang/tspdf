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
```

## Generate a QR code PDF

```bash
tspdf qrcode "https://example.com" --title "Scan me" -o qr.pdf
```

## Convert Markdown to PDF

```bash
# supports headings, lists, code blocks, pipe tables and ![alt](img.png) images
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
