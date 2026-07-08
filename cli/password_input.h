#ifndef TSPDF_PASSWORD_INPUT_H
#define TSPDF_PASSWORD_INPUT_H

// Shared password resolution for the encrypt/decrypt commands. Passwords on
// argv leak via ps / shell history / CI logs, so besides --password we accept
// --password-file <path> (first line, trailing newline stripped; '-' reads
// stdin) and fall back to an interactive no-echo prompt when stdin is a TTY.
//
// POSIX-only (termios/unistd), like server.c: Windows porting is a separate
// effort. Header-only static helpers so the Makefile source list is untouched;
// only cmd_encrypt.c and cmd_decrypt.c include this.

#include "commands.h"
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define TSPDF_PASSWORD_MAX 1024

// Read the first line of `path` into buf ('-' means stdin), stripping the
// trailing newline. Returns false (with a message on stderr) on failure.
static bool tspdf_read_password_file(const char *cmd, const char *path,
                                     char *buf, size_t buflen) {
    FILE *f;
    if (strcmp(path, "-") == 0) {
        f = stdin;
    } else {
        f = fopen(path, "r");
        if (!f) {
            fprintf(stderr, "tspdf %s: cannot open password file '%s'\n", cmd, path);
            return false;
        }
    }
    bool ok = fgets(buf, (int)buflen, f) != NULL;
    if (f != stdin) fclose(f);
    if (!ok) {
        fprintf(stderr, "tspdf %s: empty password file '%s'\n", cmd, path);
        return false;
    }
    buf[strcspn(buf, "\r\n")] = '\0';
    return true;
}

// Prompt on stderr and read a line from stdin with echo disabled. Only called
// when stdin is a TTY. Returns false on read failure (EOF).
static bool tspdf_prompt_password(const char *prompt, char *buf, size_t buflen) {
    fprintf(stderr, "%s", prompt);
    fflush(stderr);

    // TCSADRAIN (not TCSAFLUSH): flushing would discard type-ahead — a
    // password already pasted/piped in before the prompt appeared.
    struct termios saved, noecho;
    bool have_termios = tcgetattr(STDIN_FILENO, &saved) == 0;
    if (have_termios) {
        noecho = saved;
        noecho.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSADRAIN, &noecho);
    }

    bool ok = fgets(buf, (int)buflen, stdin) != NULL;

    if (have_termios) {
        tcsetattr(STDIN_FILENO, TCSADRAIN, &saved);
        fputc('\n', stderr);  // echo was off, so the user's Enter printed nothing
    }
    if (!ok) return false;
    buf[strcspn(buf, "\r\n")] = '\0';
    return true;
}

// Resolve a password from `flag` (e.g. "--password"), `file_flag` (e.g.
// "--password-file"), or — when `required` and stdin is a TTY — an interactive
// prompt. Returns the password (argv value or buf filled in), or NULL after
// printing an error. When the password is optional and absent, returns NULL
// without printing (caller applies its default).
static const char *tspdf_resolve_password(int argc, char **argv,
                                          const char *flag, const char *file_flag,
                                          const char *cmd, const char *prompt,
                                          bool required, char *buf, size_t buflen) {
    const char *pass = find_flag(argc, argv, flag);
    if (pass) return pass;

    const char *file = find_flag(argc, argv, file_flag);
    if (file) {
        return tspdf_read_password_file(cmd, file, buf, buflen) ? buf : NULL;
    }

    if (!required) return NULL;

    if (isatty(STDIN_FILENO)) {
        if (tspdf_prompt_password(prompt, buf, buflen)) return buf;
        fprintf(stderr, "tspdf %s: failed to read password\n", cmd);
        return NULL;
    }

    fprintf(stderr, "tspdf %s: no password given and stdin is not a terminal "
                    "(use %s or %s)\n", cmd, flag, file_flag);
    return NULL;
}

#endif
