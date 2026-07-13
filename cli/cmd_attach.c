// tspdf attach — embed, list, extract and remove file attachments.
//
// Extraction never trusts stored names: they come from the PDF, so a hostile
// file can claim "../../.ssh/authorized_keys". Names are reduced to their
// last path component (both '/' and '\\' separators) and bare dot names are
// rejected, so writes always land inside the target directory.

#include "commands.h"
#include "pipeline.h"
#include "password_input.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static void attach_usage(void) {
    printf("Usage: tspdf attach add <input.pdf> <file> [<file2> ...] [--desc <text>] [--mime <type>] -o <output.pdf>\n");
    printf("       tspdf attach list <input.pdf> [--json]\n");
    printf("       tspdf attach extract <input.pdf> [--name <name> | --all] [-o <dir>]\n");
    printf("       tspdf attach remove <input.pdf> --name <name> -o <output.pdf>\n");
    printf("\n");
    printf("Embed files in a PDF, or list/extract/remove embedded files.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  add <file> [...]          Files to embed (stored under their base names)\n");
    printf("  --desc <text>             Description stored with each added file\n");
    printf("  --mime <type>             MIME type stored with each added file\n");
    printf("                            (default: derived from the file extension)\n");
    printf("  list --json               Machine-readable listing\n");
    printf("  extract --name <name>     Extract one attachment by stored name\n");
    printf("  extract --all             Extract every attachment (default)\n");
    printf("  extract -o <dir>          Target directory (default: current directory)\n");
    printf("  remove --name <name>      Attachment to remove\n");
    printf("  --password <pass>         Password for encrypted PDFs (or --password-file)\n");
    printf("  -o <output.pdf>           Output file path (required for add/remove)\n");
}

// Open the input, resolving an optional --password/--password-file pair.
static TspdfReader *attach_open(int argc, char **argv, const char *input) {
    static char pwbuf[TSPDF_PASSWORD_MAX];
    const char *password = tspdf_resolve_password(argc, argv,
                                                  "--password", "--password-file",
                                                  "attach", "Password: ",
                                                  false, pwbuf, sizeof(pwbuf));

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = password
        ? tspdf_reader_open_file_with_password(input, password, &err)
        : tspdf_reader_open_file(input, &err);
    if (!doc) {
        if (err == TSPDF_ERR_ENCRYPTED) {
            fprintf(stderr, "tspdf attach: '%s' is encrypted; use --password or "
                            "--password-file\n", input);
        } else {
            fprintf(stderr, "tspdf attach: failed to open '%s': %s\n",
                    input, tspdf_error_string(err));
        }
    }
    return doc;
}

static uint8_t *attach_read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(size > 0 ? (size_t)size : 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (nread != (size_t)size) {
        free(buf);
        return NULL;
    }
    *out_len = nread;
    return buf;
}

// Last path component of `path` ('/' and '\\' both split).
static const char *attach_base_name(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

// MIME type for an added file, from its extension (case-insensitive).
// Anything unrecognized is application/octet-stream.
static const char *attach_mime_for(const char *name) {
    static const struct { const char *ext, *mime; } map[] = {
        {"txt",  "text/plain"},
        {"pdf",  "application/pdf"},
        {"csv",  "text/csv"},
        {"json", "application/json"},
        {"xml",  "application/xml"},
        {"png",  "image/png"},
        {"jpg",  "image/jpeg"},
        {"jpeg", "image/jpeg"},
        {"zip",  "application/zip"},
    };
    const char *dot = strrchr(name, '.');
    if (dot && dot[1] != '\0') {
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
            if (strcasecmp(dot + 1, map[i].ext) == 0) return map[i].mime;
        }
    }
    return "application/octet-stream";
}

// Reduce a stored attachment name to a safe file name inside the target
// directory: last path component only, and never empty or a dot name.
static const char *attach_sanitize_name(const char *stored) {
    const char *base = attach_base_name(stored);
    if (base[0] == '\0' || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
        return NULL;
    }
    return base;
}

static int attach_add(int argc, char **argv) {
    const char *positional[65];
    int npos = collect_positional(argc, argv, positional, 65);
    if (npos < 2) {
        fprintf(stderr, "tspdf attach add: need an input PDF and at least one file to attach\n");
        return 1;
    }
    if (npos > 65) {
        fprintf(stderr, "tspdf attach add: too many files (max 64 per call)\n");
        return 1;
    }
    const char *input = positional[0];
    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf attach add: missing -o <output.pdf>\n");
        return 1;
    }
    const char *desc = find_flag(argc, argv, "--desc");
    const char *mime_override = find_flag(argc, argv, "--mime");

    TspdfReader *doc = attach_open(argc, argv, input);
    if (!doc) return 1;

    for (int i = 1; i < npos; i++) {
        const char *name = attach_base_name(positional[i]);
        if (name[0] == '\0') {
            fprintf(stderr, "tspdf attach add: '%s' has no file name\n", positional[i]);
            tspdf_reader_destroy(doc);
            return 1;
        }
        size_t len = 0;
        uint8_t *data = attach_read_file(positional[i], &len);
        if (!data) {
            fprintf(stderr, "tspdf attach add: cannot read '%s'\n", positional[i]);
            tspdf_reader_destroy(doc);
            return 1;
        }
        // File metadata for /Params and /Subtype: modification time from the
        // source file, MIME type from --mime or the extension.
        struct stat st;
        int64_t mtime = 0;
        if (stat(positional[i], &st) == 0) mtime = (int64_t)st.st_mtime;
        const char *mime = mime_override ? mime_override : attach_mime_for(name);
        TspdfError err = tspdf_reader_attachment_add_ex(doc, name, data, len,
                                                        desc, mime, mtime);
        free(data);
        if (err != TSPDF_OK) {
            fprintf(stderr, "tspdf attach add: failed to attach '%s': %s\n",
                    positional[i], tspdf_error_string(err));
            tspdf_reader_destroy(doc);
            return 1;
        }
    }

    TspdfError err = tspdf_reader_save(doc, output);
    tspdf_reader_destroy(doc);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf attach add: failed to save '%s': %s\n",
                output, tspdf_error_string(err));
        return 1;
    }
    printf("Attached %d file(s) → %s\n", npos - 1, output);
    return 0;
}

static void attach_json_string(const char *s) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':  fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\b': fputs("\\b", stdout); break;
            case '\f': fputs("\\f", stdout); break;
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            default:
                if (*p < 0x20) printf("\\u%04X", *p);
                else putchar(*p);
                break;
        }
    }
    putchar('"');
}

static int attach_list(int argc, char **argv) {
    const char *positional[2];
    int npos = collect_positional(argc, argv, positional, 2);
    if (npos < 1) {
        fprintf(stderr, "tspdf attach list: missing input file\n");
        return 1;
    }
    if (npos > 1) {
        fprintf(stderr, "tspdf attach list: unexpected extra argument '%s'\n", positional[1]);
        return 1;
    }
    bool json = has_flag(argc, argv, "--json");

    TspdfReader *doc = attach_open(argc, argv, positional[0]);
    if (!doc) return 1;

    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    TspdfError err = tspdf_reader_attachments(doc, &infos, &count);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf attach list: %s\n", tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        return 1;
    }

    if (json) {
        printf("[");
        for (size_t i = 0; i < count; i++) {
            printf("%s{\"name\":", i > 0 ? "," : "");
            attach_json_string(infos[i].name);
            printf(",\"size\":%zu,\"desc\":", infos[i].size);
            if (infos[i].desc) attach_json_string(infos[i].desc);
            else fputs("null", stdout);
            printf(",\"mime\":");
            if (infos[i].mime) attach_json_string(infos[i].mime);
            else fputs("null", stdout);
            printf("}");
        }
        printf("]\n");
    } else {
        for (size_t i = 0; i < count; i++) {
            printf("%s\t%zu\t%s\n", infos[i].name, infos[i].size,
                   infos[i].desc ? infos[i].desc : "");
        }
    }

    tspdf_reader_destroy(doc);
    return 0;
}

// Write one attachment into `dir` under its sanitized stored name.
static bool attach_write_one(TspdfReader *doc, const char *stored, const char *dir) {
    const char *safe = attach_sanitize_name(stored);
    if (!safe) {
        fprintf(stderr, "tspdf attach extract: refusing unsafe attachment name '%s'\n",
                stored);
        return false;
    }

    uint8_t *data = NULL;
    size_t len = 0;
    TspdfError err = tspdf_reader_attachment_get(doc, stored, &data, &len);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf attach extract: '%s': %s\n", stored,
                tspdf_error_string(err));
        return false;
    }

    size_t path_len = strlen(dir) + 1 + strlen(safe) + 1;
    char *path = (char *)malloc(path_len);
    if (!path) {
        free(data);
        return false;
    }
    snprintf(path, path_len, "%s/%s", dir, safe);

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "tspdf attach extract: cannot write '%s'\n", path);
        free(path);
        free(data);
        return false;
    }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    free(data);
    if (written != len) {
        fprintf(stderr, "tspdf attach extract: short write on '%s'\n", path);
        free(path);
        return false;
    }
    printf("Extracted %s (%zu bytes)\n", path, len);
    free(path);
    return true;
}

static int attach_extract(int argc, char **argv) {
    const char *positional[2];
    int npos = collect_positional(argc, argv, positional, 2);
    if (npos < 1) {
        fprintf(stderr, "tspdf attach extract: missing input file\n");
        return 1;
    }
    if (npos > 1) {
        fprintf(stderr, "tspdf attach extract: unexpected extra argument '%s'\n",
                positional[1]);
        return 1;
    }
    const char *name = find_flag(argc, argv, "--name");
    const char *dir = find_flag(argc, argv, "-o");
    if (!dir) dir = ".";
    if (name && has_flag(argc, argv, "--all")) {
        fprintf(stderr, "tspdf attach extract: --name and --all are mutually exclusive\n");
        return 1;
    }

    TspdfReader *doc = attach_open(argc, argv, positional[0]);
    if (!doc) return 1;

    int rc = 0;
    if (name) {
        if (!attach_write_one(doc, name, dir)) rc = 1;
    } else {
        TspdfAttachmentInfo *infos = NULL;
        size_t count = 0;
        TspdfError err = tspdf_reader_attachments(doc, &infos, &count);
        if (err != TSPDF_OK) {
            fprintf(stderr, "tspdf attach extract: %s\n", tspdf_error_string(err));
            rc = 1;
        } else if (count == 0) {
            printf("No attachments in %s\n", positional[0]);
        } else {
            for (size_t i = 0; i < count; i++) {
                if (!attach_write_one(doc, infos[i].name, dir)) rc = 1;
            }
        }
    }

    tspdf_reader_destroy(doc);
    return rc;
}

static int attach_remove(int argc, char **argv) {
    const char *positional[2];
    int npos = collect_positional(argc, argv, positional, 2);
    if (npos < 1) {
        fprintf(stderr, "tspdf attach remove: missing input file\n");
        return 1;
    }
    if (npos > 1) {
        fprintf(stderr, "tspdf attach remove: unexpected extra argument '%s'\n",
                positional[1]);
        return 1;
    }
    const char *name = find_flag(argc, argv, "--name");
    if (!name) {
        fprintf(stderr, "tspdf attach remove: missing --name <name>\n");
        return 1;
    }
    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf attach remove: missing -o <output.pdf>\n");
        return 1;
    }

    TspdfReader *doc = attach_open(argc, argv, positional[0]);
    if (!doc) return 1;

    TspdfError err = tspdf_reader_attachment_remove(doc, name);
    if (err != TSPDF_OK) {
        if (err == TSPDF_ERR_NOT_FOUND) {
            fprintf(stderr, "tspdf attach remove: no attachment named '%s'\n", name);
        } else {
            fprintf(stderr, "tspdf attach remove: %s\n", tspdf_error_string(err));
        }
        tspdf_reader_destroy(doc);
        return 1;
    }

    err = tspdf_reader_save(doc, output);
    tspdf_reader_destroy(doc);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf attach remove: failed to save '%s': %s\n",
                output, tspdf_error_string(err));
        return 1;
    }
    printf("Removed '%s' → %s\n", name, output);
    return 0;
}

int cmd_attach(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        attach_usage();
        return argc == 0 ? 1 : 0;
    }

    const char *verb = argv[0];
    int sub_argc = argc - 1;
    char **sub_argv = argv + 1;

    if (strcmp(verb, "add") == 0)     return attach_add(sub_argc, sub_argv);
    if (strcmp(verb, "list") == 0)    return attach_list(sub_argc, sub_argv);
    if (strcmp(verb, "extract") == 0) return attach_extract(sub_argc, sub_argv);
    if (strcmp(verb, "remove") == 0)  return attach_remove(sub_argc, sub_argv);

    fprintf(stderr, "tspdf attach: unknown subcommand '%s'\n\n", verb);
    attach_usage();
    return 1;
}


/* attach dispatches on a subcommand (add/list/extract/remove) — it stays a RAW_ARGS command. */
static int run_attach_raw(TspdfCmdCtx *ctx) { return cmd_attach(ctx->argc, ctx->argv); }
static const TspdfCliFlag ATTACH_FLAGS[] = {
    {"-o", true},
    {"--desc", true},
    {"--mime", true},
    {"--name", true},
    {"--password", true},
    {"--password-file", true},
    {NULL, false}
};
const TspdfCmdSpec tspdf_cmd_attach_spec = {
    .name = "attach",
    .flags = ATTACH_FLAGS,
    .min_pos = 0, .max_pos = -1,
    .needs = TSPDF_CMD_RAW_ARGS,
    .run = run_attach_raw,
};
