#ifndef TSPDF_PIPELINE_H
#define TSPDF_PIPELINE_H

// Command-spec pipeline. Every standard command re-implements the same shell:
// a --help block, positional collection with count validation, -o handling,
// password resolution, document open (with the encrypted / failed-open error
// messages), and destroy on every path. This module owns that shell so the
// cmd_*.c bodies keep only their command-specific work.
//
// A command registers a TspdfCmdSpec (name, usage text, its flags, positional
// arity, and a `needs` bitmask). The union of every spec's value-taking flags
// is what is_value_flag() consults — the handwritten VALUE_FLAGS array is gone,
// so a value flag can no longer be forgotten and silently corrupt positional
// collection.

#include <stddef.h>
#include <stdbool.h>

#include "../include/tspdf.h"

// A single flag a command understands. `takes_value` marks flags whose
// following argv token is a value (e.g. `-o out.pdf`, `--pages 1-3`), not a
// positional — the pipeline needs this to collect positionals correctly.
typedef struct {
    const char *name;    // "--pages", "-o", ...
    bool takes_value;
} TspdfCliFlag;

// `needs` bits.
enum {
    TSPDF_CMD_OPENS_INPUT   = 1u << 0,  // open positional[0] into ctx->doc
    TSPDF_CMD_NEEDS_OUTPUT  = 1u << 1,  // require -o; store in ctx->output
    TSPDF_CMD_TAKES_PASSWORD = 1u << 2, // resolve --password/--password-file
    // The command keeps its own argc/argv body; the pipeline only supplies
    // name/usage/flags (for value-flag derivation) and help printing. Used by
    // the nonstandard shapes (serve, img2pdf, merge, stamp, form, ...).
    TSPDF_CMD_RAW_ARGS      = 1u << 3,
    // Password resolution is required (encrypt/decrypt): prompt/error when no
    // password is given, instead of falling through to an unencrypted open.
    TSPDF_CMD_PASSWORD_REQUIRED = 1u << 4,
};

typedef struct TspdfCmdSpec TspdfCmdSpec;

// The parsed request handed to a command's run() callback.
typedef struct {
    const TspdfCmdSpec *spec;
    int argc;            // command-specific argv (past the subcommand name)
    char **argv;

    const char *positional[8];  // collected positionals (capped at max_pos+1)
    int npos;

    const char *input;   // positional[0] when max_pos >= 1
    const char *output;  // -o value (NULL unless NEEDS_OUTPUT resolved it)
    const char *password;// resolved password (may be NULL)

    TspdfReader *doc;    // opened input when OPENS_INPUT (pipeline destroys it)
} TspdfCmdCtx;

struct TspdfCmdSpec {
    const char *name;              // "rotate"
    const char *usage;             // full --help text (printed verbatim)
    const TspdfCliFlag *flags;     // NULL-terminated flag list
    int min_pos, max_pos;          // positional arity (max_pos == -1: unbounded)
    unsigned needs;                // OPENS_INPUT | NEEDS_OUTPUT | ...
    int (*run)(TspdfCmdCtx *ctx);  // command body
};

// True if `arg` is a value-taking flag of any registered command. Replaces the
// old handwritten VALUE_FLAGS lookup; derived from the spec table.
bool tspdf_cli_is_value_flag(const char *arg);

// Look up a spec by command name, or NULL.
const TspdfCmdSpec *tspdf_cli_find_spec(const char *name);

// Run a command through the pipeline. Returns the process exit code, or -1 if
// `name` is not a registered command (so main.c can print "unknown command").
int tspdf_cli_run(const char *name, int argc, char **argv);

// Print a command's usage text (delegated to by `help <cmd>`). Returns false
// if the command is unknown.
bool tspdf_cli_print_help(const char *name);

#endif
