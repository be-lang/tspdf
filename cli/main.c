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
    printf("  crop       Set the CropBox of pages (clip the visible region)\n");
    printf("  scale      Resize pages, scaling their content\n");
    printf("\n");
    printf("Options:\n");
    printf("  --help, -h     Show this help message\n");
    printf("  --version      Show version information\n");
    printf("\n");
    printf("Run 'tspdf help <command>' for command-specific help.\n");
}

static const struct {
    const char *name;
    int (*fn)(int argc, char **argv);
} commands[] = {
    { "merge",     cmd_merge },
    { "split",     cmd_split },
    { "rotate",    cmd_rotate },
    { "delete",    cmd_delete },
    { "reorder",   cmd_reorder },
    { "encrypt",   cmd_encrypt },
    { "decrypt",   cmd_decrypt },
    { "metadata",  cmd_metadata },
    { "info",      cmd_info },
    { "watermark", cmd_watermark },
    { "compress",  cmd_compress },
    { "img2pdf",   cmd_img2pdf },
    { "qrcode",    cmd_qrcode },
    { "md2pdf",    cmd_md2pdf },
    { "serve",     cmd_serve },
    { "text",      cmd_text },
    { "pagenum",   cmd_pagenum },
    { "form",      cmd_form },
    { "attach",    cmd_attach },
    { "bookmark",  cmd_bookmark },
    { "stamp",     cmd_stamp },
    { "nup",       cmd_nup },
    { "crop",      cmd_crop },
    { "scale",     cmd_scale },
};

/* Run a subcommand; returns -1 for an unknown command name. */
static int run_command(const char *cmd, int argc, char **argv) {
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        if (strcmp(cmd, commands[i].name) == 0)
            return commands[i].fn(argc, argv);
    return -1;
}

static void print_command_help(const char *cmd) {
    /* Delegate to the command's own --help so the text has a single
     * source of truth (the cmd_*.c usage next to the flag parsing). */
    char *help_argv[] = { (char *)"--help" };
    if (run_command(cmd, 1, help_argv) < 0)
        print_usage(); /* unknown command — fall back to general help */
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

    int ret = run_command(cmd, sub_argc, sub_argv);
    if (ret >= 0)
        return ret;

    fprintf(stderr, "tspdf: unknown command '%s'\n\n", cmd);
    print_usage();
    return 1;
}
