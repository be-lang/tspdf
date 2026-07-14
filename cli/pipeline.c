#include "pipeline.h"
#include "commands.h"
#include "password_input.h"

#include <stdio.h>
#include <string.h>

// Every command's spec. Declared in the cmd_*.c file next to its run body.
#define SPEC(n) extern const TspdfCmdSpec tspdf_cmd_##n##_spec;
SPEC(merge) SPEC(split) SPEC(rotate) SPEC(delete) SPEC(reorder)
SPEC(encrypt) SPEC(decrypt) SPEC(metadata) SPEC(info) SPEC(watermark)
SPEC(compress) SPEC(img2pdf) SPEC(qrcode) SPEC(md2pdf) SPEC(serve)
SPEC(text) SPEC(pagenum) SPEC(form) SPEC(attach) SPEC(bookmark)
SPEC(stamp) SPEC(nup) SPEC(crop) SPEC(scale)
#undef SPEC

static const TspdfCmdSpec *const SPECS[] = {
    &tspdf_cmd_merge_spec,     &tspdf_cmd_split_spec,
    &tspdf_cmd_rotate_spec,    &tspdf_cmd_delete_spec,
    &tspdf_cmd_reorder_spec,   &tspdf_cmd_encrypt_spec,
    &tspdf_cmd_decrypt_spec,   &tspdf_cmd_metadata_spec,
    &tspdf_cmd_info_spec,      &tspdf_cmd_watermark_spec,
    &tspdf_cmd_compress_spec,  &tspdf_cmd_img2pdf_spec,
    &tspdf_cmd_qrcode_spec,    &tspdf_cmd_md2pdf_spec,
    &tspdf_cmd_serve_spec,     &tspdf_cmd_text_spec,
    &tspdf_cmd_pagenum_spec,   &tspdf_cmd_form_spec,
    &tspdf_cmd_attach_spec,    &tspdf_cmd_bookmark_spec,
    &tspdf_cmd_stamp_spec,     &tspdf_cmd_nup_spec,
    &tspdf_cmd_crop_spec,      &tspdf_cmd_scale_spec,
};
#define SPEC_COUNT ((int)(sizeof(SPECS) / sizeof(SPECS[0])))

bool tspdf_cli_is_value_flag(const char *arg) {
    if (!arg) return false;
    for (size_t s = 0; s < SPEC_COUNT; s++) {
        const TspdfCliFlag *f = SPECS[s]->flags;
        for (; f && f->name; f++) {
            if (f->takes_value && strcmp(arg, f->name) == 0) return true;
        }
    }
    return false;
}

const TspdfCmdSpec *tspdf_cli_find_spec(const char *name) {
    for (size_t s = 0; s < SPEC_COUNT; s++)
        if (strcmp(name, SPECS[s]->name) == 0) return SPECS[s];
    return NULL;
}

bool tspdf_cli_print_help(const char *name) {
    const TspdfCmdSpec *spec = tspdf_cli_find_spec(name);
    if (!spec) return false;
    // RAW commands own their help text in their body; delegate to it with a
    // synthetic --help so the text has a single source of truth.
    if (spec->needs & TSPDF_CMD_RAW_ARGS) {
        char *help_argv[] = { (char *)"--help" };
        TspdfCmdCtx ctx = {0};
        ctx.spec = spec;
        ctx.argc = 1;
        ctx.argv = help_argv;
        spec->run(&ctx);
    } else {
        fputs(spec->usage, stdout);
    }
    return true;
}

int tspdf_cli_run(const char *name, int argc, char **argv) {
    const TspdfCmdSpec *spec = tspdf_cli_find_spec(name);
    if (!spec) return -1;

    // Nonstandard commands keep their own argc/argv body, including their own
    // --help / no-args handling (some, like serve, run with no arguments).
    if (spec->needs & TSPDF_CMD_RAW_ARGS) {
        TspdfCmdCtx ctx = {0};
        ctx.spec = spec;
        ctx.argc = argc;
        ctx.argv = argv;
        return spec->run(&ctx);
    }

    // --help / no args: print usage. Exit 1 when invoked with no arguments at
    // all (a usage error), 0 for an explicit --help.
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        fputs(spec->usage, stdout);
        return argc == 0 ? 1 : 0;
    }

    TspdfCmdCtx ctx = {0};
    ctx.spec = spec;
    ctx.argc = argc;
    ctx.argv = argv;

    // Positional collection + arity validation, using the exact legacy strings.
    int cap = spec->max_pos >= 0 ? spec->max_pos + 1 : (int)8;
    if (cap > 8) cap = 8;
    ctx.npos = collect_positional(argc, argv, ctx.positional, cap);
    if (ctx.npos < spec->min_pos) {
        fprintf(stderr, "tspdf %s: missing input file\n", spec->name);
        return 1;
    }
    if (spec->max_pos >= 0 && ctx.npos > spec->max_pos) {
        fprintf(stderr, "tspdf %s: unexpected extra argument '%s'\n",
                spec->name, ctx.positional[spec->max_pos]);
        return 1;
    }
    if (spec->max_pos >= 1) ctx.input = ctx.positional[0];

    // -o is resolved but only *required* when the command always needs it.
    ctx.output = find_flag(argc, argv, "-o");
    if ((spec->needs & TSPDF_CMD_NEEDS_OUTPUT) && !ctx.output) {
        fprintf(stderr, "tspdf %s: missing -o <output.pdf>\n", spec->name);
        return 1;
    }

    // Password resolution.
    static char pwbuf[TSPDF_PASSWORD_MAX];
    if (spec->needs & TSPDF_CMD_TAKES_PASSWORD) {
        bool required = (spec->needs & TSPDF_CMD_PASSWORD_REQUIRED) != 0;
        ctx.password = tspdf_resolve_password(argc, argv,
                                              "--password", "--password-file",
                                              spec->name, "Password: ",
                                              required, pwbuf, sizeof(pwbuf));
        if (required && !ctx.password) return 1;
    }

    // Open the input document.
    if (spec->needs & TSPDF_CMD_OPENS_INPUT) {
        ctx.doc = tspdf_cli_open_input(spec->name, ctx.input, ctx.password,
                                       NULL, NULL, NULL);
        if (!ctx.doc) return 1;
    }

    int rc = spec->run(&ctx);

    if (ctx.doc) tspdf_reader_destroy(ctx.doc);
    return rc;
}
