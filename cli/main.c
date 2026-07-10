#include "commands.h"
#include "../include/tspdf/version.h"

#include <stdio.h>
#include <string.h>

static void print_usage(void) {
    printf("tspdf v" TSPDF_VERSION_STRING " — PDF toolkit\n");
    printf("\n");
    printf("Usage: tspdf <command> [options]\n");
    printf("\n");
    printf("Commands:\n");
    printf("  merge      Merge multiple PDF files into one\n");
    printf("  split      Split a PDF into individual pages or ranges\n");
    printf("  rotate     Rotate pages in a PDF\n");
    printf("  delete     Delete pages from a PDF\n");
    printf("  reorder    Reorder pages in a PDF\n");
    printf("  encrypt    Encrypt a PDF with a password\n");
    printf("  decrypt    Decrypt a password-protected PDF\n");
    printf("  metadata   View or edit PDF metadata\n");
    printf("  info       Print information about a PDF\n");
    printf("  watermark  Add a text or image watermark to a PDF\n");
    printf("  compress   Compress a PDF to reduce file size\n");
    printf("  img2pdf    Convert images (JPEG/PNG) to a PDF\n");
    printf("  qrcode     Generate a QR code PDF\n");
    printf("  md2pdf     Convert Markdown to a styled PDF\n");
    printf("  serve      Start a local web server for PDF tools\n");
    printf("  text       Extract text from a PDF\n");
    printf("  pagenum    Stamp page numbers onto a PDF\n");
    printf("  form       List, fill, or flatten PDF form fields\n");
    printf("  attach     Embed, list, extract, or remove file attachments\n");
    printf("  bookmark   List, add, import, or clear PDF outlines (bookmarks)\n");
    printf("  stamp      Overlay a PDF page onto another PDF's pages\n");
    printf("  nup        Place multiple pages per sheet (2-up, 4-up, ...)\n");
    printf("\n");
    printf("Options:\n");
    printf("  --help, -h     Show this help message\n");
    printf("  --version      Show version information\n");
    printf("\n");
    printf("Run 'tspdf help <command>' for command-specific help.\n");
}

static void print_command_help(const char *cmd) {
    if (strcmp(cmd, "merge") == 0) {
        printf("Usage: tspdf merge <file1.pdf> <file2.pdf> [...] -o <output.pdf>\n");
        printf("\n");
        printf("Combine multiple PDFs into a single document.\n");
        printf("Pages are concatenated in the order given.\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  <file1.pdf> <file2.pdf> [...]  Two or more input PDF files\n");
        printf("  -o <output.pdf>               Output file path (required)\n");
    } else if (strcmp(cmd, "split") == 0) {
        printf("Usage: tspdf split <input.pdf> [--pages <range> | --burst] -o <output.pdf>\n");
        printf("\n");
        printf("Extract specific pages from a PDF, or split it into one file per page.\n");
        printf("Without --pages, every page is written to its own file (burst mode):\n");
        printf("out.pdf becomes out-001.pdf, out-002.pdf, ...\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  <input.pdf>       Input PDF file\n");
        printf("  --pages <range>   Page range to extract, e.g. 1-3 or 1,3,5 or 2-4,7\n");
        printf("  --burst           One output file per page (default without --pages)\n");
        printf("  -o <output.pdf>   Output file path (required)\n");
    } else if (strcmp(cmd, "rotate") == 0) {
        printf("Usage: tspdf rotate <input.pdf> --angle <deg> [-o <output.pdf>] [--pages <range>]\n");
        printf("\n");
        printf("Rotate pages in a PDF. Angle must be 90, 180, or 270.\n");
        printf("If --pages is omitted, all pages are rotated.\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  <input.pdf>       Input PDF file\n");
        printf("  --angle <deg>     Rotation angle: 90, 180, or 270\n");
        printf("  --pages <range>   Pages to rotate (optional, default: all)\n");
        printf("  -o <output.pdf>   Output file path (required)\n");
    } else if (strcmp(cmd, "delete") == 0) {
        printf("Usage: tspdf delete <input.pdf> --pages <range> -o <output.pdf>\n");
        printf("\n");
        printf("Remove specific pages from a PDF.\n");
        printf("The remaining pages are written to the output file.\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  <input.pdf>       Input PDF file\n");
        printf("  --pages <range>   Pages to delete, e.g. 2,4 or 1-3\n");
        printf("  -o <output.pdf>   Output file path (required)\n");
    } else if (strcmp(cmd, "reorder") == 0) {
        printf("Usage: tspdf reorder <input.pdf> --order <page,page,...> -o <output.pdf>\n");
        printf("\n");
        printf("Reorder pages in a PDF.\n");
        printf("The --order list must contain every page number exactly once.\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  <input.pdf>              Input PDF file\n");
        printf("  --order <page,page,...>  New page order, e.g. 3,1,2\n");
        printf("  -o <output.pdf>          Output file path (required)\n");
    } else if (strcmp(cmd, "encrypt") == 0) {
        printf("Usage: tspdf encrypt <input.pdf> -o <output.pdf> --password <pass>\n");
        printf("                     [--owner-password <pass>] [--bits 128|256]\n");
        printf("\n");
        printf("Encrypt a PDF with AES encryption.\n");
        printf("Default key size is 128-bit; use --bits 256 for AES-256.\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  <input.pdf>              Input PDF file\n");
        printf("  --password <pass>        User password (required)\n");
        printf("  --owner-password <pass>  Owner password (optional, defaults to user password)\n");
        printf("  --bits 128|256           Key size in bits (optional, default: 128)\n");
        printf("  -o <output.pdf>          Output file path (required)\n");
    } else if (strcmp(cmd, "decrypt") == 0) {
        printf("Usage: tspdf decrypt <input.pdf> -o <output.pdf> --password <pass>\n");
        printf("\n");
        printf("Remove encryption from a password-protected PDF.\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  <input.pdf>        Input PDF file\n");
        printf("  --password <pass>  Password to open the PDF (required)\n");
        printf("  -o <output.pdf>    Output file path (required)\n");
    } else if (strcmp(cmd, "metadata") == 0) {
        printf("Usage: tspdf metadata <input.pdf>                                      # view\n");
        printf("       tspdf metadata <input.pdf> --set key=value [...] -o <out.pdf>   # edit\n");
        printf("\n");
        printf("View or edit PDF metadata fields.\n");
        printf("Without --set flags the current metadata is printed to stdout.\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  <input.pdf>          Input PDF file\n");
        printf("  --set key=value      Set a metadata field (repeatable)\n");
        printf("                       Keys: title, author, subject, keywords, creator\n");
        printf("  -o <output.pdf>      Output file path (required when using --set)\n");
    } else if (strcmp(cmd, "info") == 0) {
        printf("Usage: tspdf info <input.pdf> [--password <pass>]\n");
        printf("\n");
        printf("Print information about a PDF file.\n");
        printf("Shows page count, page dimensions, and encryption status.\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  <input.pdf>        Input PDF file\n");
        printf("  --password <pass>  Password for encrypted PDFs (optional)\n");
    } else if (strcmp(cmd, "watermark") == 0) {
        printf("Usage: tspdf watermark <input.pdf> -o <output.pdf> --text <text>\n");
        printf("                       [--opacity <0.0-1.0>]\n");
        printf("       tspdf watermark <input.pdf> -o <output.pdf> --image <logo.png|jpg>\n");
        printf("                       [--opacity <0.0-1.0>] [--scale <factor>] [--position <pos>]\n");
        printf("\n");
        printf("Add a watermark to all pages of a PDF: a diagonal text stamp, or an\n");
        printf("image (PNG or JPEG). --text and --image are mutually exclusive.\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  <input.pdf>         Input PDF file\n");
        printf("  --text <text>       Watermark text, e.g. \"DRAFT\"\n");
        printf("  --image <file>      Watermark image (PNG or JPEG)\n");
        printf("  --opacity <float>   Opacity from 0.0 (invisible) to 1.0 (opaque), default: 0.3\n");
        printf("  --scale <factor>    Image size: larger image dimension = scale x the\n");
        printf("                      smaller page dimension (default: 0.5)\n");
        printf("  --position <pos>    center (default), tile, top-left, top-right,\n");
        printf("                      bottom-left, bottom-right\n");
        printf("  -o <output.pdf>     Output file path (required)\n");
    } else if (strcmp(cmd, "stamp") == 0) {
        printf("Usage: tspdf stamp <input.pdf> --stamp <stamp.pdf> -o <output.pdf>\n");
        printf("                   [--pages <range>] [--under] [--stamp-page N]\n");
        printf("                   [--password <pass>]\n");
        printf("\n");
        printf("Overlay a page of stamp.pdf onto pages of input.pdf (like pdftk\n");
        printf("stamp / qpdf overlay). The stamp page is scaled to fit each target\n");
        printf("page, keeping its aspect ratio, and centered.\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  <input.pdf>         Input PDF file\n");
        printf("  --stamp <stamp.pdf> PDF whose page is stamped onto the input (required)\n");
        printf("  --stamp-page N      Which page of stamp.pdf to use (default: 1)\n");
        printf("  --pages <range>     Pages to stamp, e.g. 1-3 or 2,4 (default: all)\n");
        printf("  --under             Draw the stamp beneath the existing content\n");
        printf("                      (letterhead/background) instead of on top\n");
        printf("  --password <pass>   Password for an encrypted input (output is saved\n");
        printf("                      decrypted; re-encrypt with 'tspdf encrypt')\n");
        printf("  -o <output.pdf>     Output file path (required)\n");
    } else if (strcmp(cmd, "nup") == 0) {
        printf("Usage: tspdf nup <N> <input.pdf> -o <output.pdf>\n");
        printf("                 [--pages <range>] [--page-size a4|letter|source]\n");
        printf("                 [--landscape] [--gap <pt>] [--frame] [--password <pass>]\n");
        printf("\n");
        printf("Place N source pages onto each output sheet in a grid (imposition,\n");
        printf("like pdfjam/pdftk). Each page is scaled to fit its cell, keeping its\n");
        printf("aspect ratio, and centered, in reading order (left-to-right, top-to-bottom).\n");
        printf("Only page content is kept; bookmarks, forms, and annotations are dropped.\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  <N>                 Pages per sheet: 2, 4, 6, 8, 9, or 16\n");
        printf("  <input.pdf>         Input PDF file\n");
        printf("  --pages <range>     Pages to impose, e.g. 1-3 or 2,4 (default: all)\n");
        printf("  --page-size <s>     Sheet size: a4 (default), letter, or source\n");
        printf("                      (source sizes the sheet as a grid of the first page)\n");
        printf("  --landscape         Swap the sheet width and height\n");
        printf("  --gap <pt>          Gap in points between cells (default: 0)\n");
        printf("  --frame             Draw a thin border around each cell\n");
        printf("  --password <pass>   Password for an encrypted input (or --password-file)\n");
        printf("  -o <output.pdf>     Output file path (required)\n");
    } else if (strcmp(cmd, "compress") == 0) {
        printf("Usage: tspdf compress <input.pdf> -o <output.pdf>\n");
        printf("\n");
        printf("Compress a PDF to reduce file size.\n");
        printf("Applies stream compression to content streams that are not already compressed.\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  <input.pdf>      Input PDF file\n");
        printf("  -o <output.pdf>  Output file path (required)\n");
    } else if (strcmp(cmd, "img2pdf") == 0) {
        printf("Usage: tspdf img2pdf <image1.jpg> <image2.png> [...] -o <output.pdf>\n");
        printf("                     [--page-size a4|letter|image]\n");
        printf("\n");
        printf("Convert JPEG and PNG images into a multi-page PDF.\n");
        printf("Each image is placed on its own page, scaled to fit with margins.\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  <image1> <image2> [...]  One or more JPEG/PNG image files\n");
        printf("  -o <output.pdf>          Output file path (required)\n");
        printf("  --page-size <size>       a4 (default), letter, or image (page sized\n");
        printf("                           to each image at 72 dpi, no margin)\n");
    } else if (strcmp(cmd, "qrcode") == 0) {
        printf("Usage: tspdf qrcode <text> -o <output.pdf> [--title <title>] [--subtitle <sub>]\n");
        printf("\n");
        printf("Generate a PDF containing a QR code for the given text or URL.\n");
        printf("The QR code is centered on an A4 page with title and text labels.\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  <text>               Text or URL to encode\n");
        printf("  -o <output.pdf>      Output file path (required)\n");
        printf("  --title <title>      Title above the QR code (default: \"QR Code\")\n");
        printf("  --subtitle <text>    Subtitle below the URL (optional)\n");
    } else if (strcmp(cmd, "md2pdf") == 0) {
        printf("Usage: tspdf md2pdf <input.md> -o <output.pdf>\n");
        printf("\n");
        printf("Convert a Markdown document into a styled PDF.\n");
        printf("Supports headings, paragraphs, lists, code blocks, blockquotes, rules,\n");
        printf("pipe tables, and block-level ![alt](path) images (JPEG/PNG).\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  <input.md>       Input Markdown file\n");
        printf("  -o <output.pdf>  Output file path (required)\n");
    } else if (strcmp(cmd, "serve") == 0) {
        printf("Usage: tspdf serve [--port <port>] [--bind <addr>]\n");
        printf("\n");
        printf("Start a local web server for PDF tools.\n");
        printf("Default port: 8080, default bind address: 127.0.0.1\n");
        printf("Binding a non-loopback address exposes the unauthenticated UI\n");
        printf("to the network; a warning is printed.\n");
    } else if (strcmp(cmd, "text") == 0) {
        printf("Usage: tspdf text <input.pdf> [--layout] [--pages <range>] [--password <pass>] [-o <output.txt>]\n");
        printf("\n");
        printf("Extract text from a PDF, in content-stream order.\n");
        printf("All pages go to stdout by default, separated by form-feed (\\f).\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  <input.pdf>        Input PDF file\n");
        printf("  --layout           Preserve the page layout (columns and tables\n");
        printf("                     stay aligned, like pdftotext -layout)\n");
        printf("  --pages <range>    Pages to extract, e.g. 1-3 or 1,3,5 (default: all)\n");
        printf("  -o <output.txt>    Write to a file instead of stdout\n");
        printf("  --password <pass>  Password for encrypted PDFs (optional)\n");
    } else if (strcmp(cmd, "pagenum") == 0) {
        printf("Usage: tspdf pagenum <input.pdf> -o <output.pdf> [--format \"%%d / %%d\"]\n");
        printf("                     [--position <pos>] [--start N] [--font-size N]\n");
        printf("                     [--pages <range>]\n");
        printf("\n");
        printf("Stamp a page number on every page of a PDF.\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  <input.pdf>        Input PDF file\n");
        printf("  --format <fmt>     Label format; up to two %%d (page number, total).\n");
        printf("                     Default: \"%%d\"\n");
        printf("  --position <pos>   bottom-center (default), bottom-left, bottom-right,\n");
        printf("                     top-center, top-left, top-right\n");
        printf("  --start N          Number of the first page (default: 1)\n");
        printf("  --font-size N      Font size in points (default: 10)\n");
        printf("  --pages <range>    Only stamp these pages, e.g. 2-10 or 2,4 (default:\n");
        printf("                     all). Numbering still reflects the true position.\n");
        printf("  -o <output.pdf>    Output file path (required)\n");
    } else if (strcmp(cmd, "form") == 0) {
        printf("Usage: tspdf form <list|fill|flatten> <input.pdf> [options]\n");
        printf("\n");
        printf("Work with AcroForm form fields (interactive PDF forms).\n");
        printf("\n");
        printf("Subcommands:\n");
        printf("  list <input.pdf>                    Print all fields as a JSON array:\n");
        printf("                                      name, type, value, page, required,\n");
        printf("                                      readonly, and options where relevant\n");
        printf("  fill <input.pdf> -o <output.pdf>    Set field values\n");
        printf("      --data <file.json|->            JSON object {\"field\": \"value\", ...}\n");
        printf("                                      ('-' reads it from stdin)\n");
        printf("      --set name=value                Set one field (repeatable)\n");
        printf("      --force                         Also fill readonly fields\n");
        printf("  flatten <input.pdf> -o <output.pdf> Stamp current values into the page\n");
        printf("                                      content and remove the form\n");
        printf("\n");
        printf("Options:\n");
        printf("  --password <pass>  Password for encrypted PDFs (optional)\n");
        printf("\n");
        printf("Checkboxes and radio groups accept an \"on\" state name (see options in\n");
        printf("`form list`), \"Off\", or true/false. Filled text fields get a single-line\n");
        printf("appearance (no comb or multiline layout).\n");
    } else if (strcmp(cmd, "attach") == 0) {
        printf("Usage: tspdf attach add <input.pdf> <file> [<file2> ...] [--desc <text>] -o <output.pdf>\n");
        printf("       tspdf attach list <input.pdf> [--json]\n");
        printf("       tspdf attach extract <input.pdf> [--name <name> | --all] [-o <dir>]\n");
        printf("       tspdf attach remove <input.pdf> --name <name> -o <output.pdf>\n");
        printf("\n");
        printf("Embed files in a PDF, or list/extract/remove embedded files.\n");
        printf("Attachments are document-level and survive split/merge/rotate.\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  add <file> [...]       Files to embed (stored under their base names)\n");
        printf("  --desc <text>          Description stored with each added file\n");
        printf("  list --json            Machine-readable listing\n");
        printf("  extract --name <name>  Extract one attachment by stored name\n");
        printf("  extract --all          Extract every attachment (default)\n");
        printf("  extract -o <dir>       Target directory (default: current directory)\n");
        printf("  remove --name <name>   Attachment to remove\n");
        printf("  --password <pass>      Password for encrypted PDFs (or --password-file)\n");
        printf("  -o <output.pdf>        Output file path (required for add/remove)\n");
    } else if (strcmp(cmd, "bookmark") == 0) {
        printf("Usage: tspdf bookmark list <input.pdf> [--json]\n");
        printf("       tspdf bookmark add <input.pdf> --title <text> --page <N> [--level <L>] -o <output.pdf>\n");
        printf("       tspdf bookmark import <input.pdf> --from <toc.txt|-> -o <output.pdf>\n");
        printf("       tspdf bookmark clear <input.pdf> -o <output.pdf>\n");
        printf("\n");
        printf("Edit the outline (bookmarks) of an existing PDF.\n");
        printf("\n");
        printf("The TOC file for import has one entry per line, TAB-separated:\n");
        printf("  LEVEL<TAB>PAGE<TAB>TITLE\n");
        printf("LEVEL is the 1-based nesting depth (level 1 = top; no jumps > 1),\n");
        printf("PAGE is the 1-based page number. Blank lines and '#' comments are\n");
        printf("ignored. This is exactly what `bookmark list` prints, so a listing\n");
        printf("can be edited and re-imported.\n");
        printf("\n");
        printf("Arguments:\n");
        printf("  list --json            Machine-readable listing (title/level/page)\n");
        printf("  add --title <text>     Bookmark title to append\n");
        printf("  add --page <N>         Target page (1-based)\n");
        printf("  add --level <L>        Nesting level (1-based, default 1)\n");
        printf("  import --from <file>   TOC file to import (- reads stdin)\n");
        printf("  --password <pass>      Password for encrypted PDFs (or --password-file)\n");
        printf("  -o <output.pdf>        Output file (required for add/import/clear)\n");
    } else {
        /* Unknown command — fall back to general help */
        print_usage();
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 0;
    }

    const char *cmd = argv[1];

    // Help / version flags at top level
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_usage();
        return 0;
    }

    if (strcmp(cmd, "help") == 0) {
        /* "tspdf help <command>" */
        if (argc >= 3) {
            print_command_help(argv[2]);
        } else {
            print_usage();
        }
        return 0;
    }

    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "version") == 0) {
        printf("tspdf v" TSPDF_VERSION_STRING "\n");
        return 0;
    }

    // Shift argc/argv past the subcommand name
    int sub_argc = argc - 2;
    char **sub_argv = argv + 2;

    if (strcmp(cmd, "merge") == 0)    return cmd_merge(sub_argc, sub_argv);
    if (strcmp(cmd, "split") == 0)    return cmd_split(sub_argc, sub_argv);
    if (strcmp(cmd, "rotate") == 0)   return cmd_rotate(sub_argc, sub_argv);
    if (strcmp(cmd, "delete") == 0)   return cmd_delete(sub_argc, sub_argv);
    if (strcmp(cmd, "reorder") == 0)  return cmd_reorder(sub_argc, sub_argv);
    if (strcmp(cmd, "encrypt") == 0)  return cmd_encrypt(sub_argc, sub_argv);
    if (strcmp(cmd, "decrypt") == 0)  return cmd_decrypt(sub_argc, sub_argv);
    if (strcmp(cmd, "metadata") == 0) return cmd_metadata(sub_argc, sub_argv);
    if (strcmp(cmd, "info") == 0)     return cmd_info(sub_argc, sub_argv);
    if (strcmp(cmd, "watermark") == 0) return cmd_watermark(sub_argc, sub_argv);
    if (strcmp(cmd, "compress") == 0) return cmd_compress(sub_argc, sub_argv);
    if (strcmp(cmd, "img2pdf") == 0)  return cmd_img2pdf(sub_argc, sub_argv);
    if (strcmp(cmd, "qrcode") == 0)   return cmd_qrcode(sub_argc, sub_argv);
    if (strcmp(cmd, "md2pdf") == 0)   return cmd_md2pdf(sub_argc, sub_argv);
    if (strcmp(cmd, "serve") == 0)   return cmd_serve(sub_argc, sub_argv);
    if (strcmp(cmd, "text") == 0)    return cmd_text(sub_argc, sub_argv);
    if (strcmp(cmd, "pagenum") == 0)  return cmd_pagenum(sub_argc, sub_argv);
    if (strcmp(cmd, "form") == 0)     return cmd_form(sub_argc, sub_argv);
    if (strcmp(cmd, "attach") == 0)   return cmd_attach(sub_argc, sub_argv);
    if (strcmp(cmd, "bookmark") == 0) return cmd_bookmark(sub_argc, sub_argv);
    if (strcmp(cmd, "stamp") == 0)    return cmd_stamp(sub_argc, sub_argv);
    if (strcmp(cmd, "nup") == 0)      return cmd_nup(sub_argc, sub_argv);

    fprintf(stderr, "tspdf: unknown command '%s'\n\n", cmd);
    print_usage();
    return 1;
}
