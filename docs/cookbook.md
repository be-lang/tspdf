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

## Troubleshooting quick checks

```bash
# show command-specific help
tspdf help merge

# print version
tspdf --version
```
