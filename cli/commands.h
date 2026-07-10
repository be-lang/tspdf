#ifndef TSPDF_COMMANDS_H
#define TSPDF_COMMANDS_H

#include <stddef.h>
#include <stdbool.h>

// Parse "--pages 1,3-5,7" into zero-indexed array. Caller frees. NULL on error.
size_t *parse_page_range(const char *spec, size_t *out_count);

// Find flag value in argv. Returns arg after flag, or NULL if not found.
const char *find_flag(int argc, char **argv, const char *flag);

// Check if flag exists in argv (boolean flag, no value).
bool has_flag(int argc, char **argv, const char *flag);

// Collect all values of a repeated flag (e.g., multiple --set).
// Returns count. Values stored in out[] (max out_max entries).
int find_flags(int argc, char **argv, const char *flag, const char **out, int out_max);

// Collect positional args (not flags). Returns the TOTAL number of
// positionals found, even when it exceeds out_max; only the first out_max
// are stored in out[]. Callers must compare the return value against the
// number of inputs they expect instead of assuming it was capped.
int collect_positional(int argc, char **argv, const char **out, int out_max);

// First page index in pages[] that is >= total, or total if none is.
size_t first_out_of_range(const size_t *pages, size_t count, size_t total);

// Concat a rotation by `rot` degrees CCW about (cx, cy) into a content
// stream — compensates a page-level /Rotate so drawing reads upright as
// viewed. No-op for rot == 0. Defined in cmd_stamp.c.
struct TspdfStream;
void tspdf_cli_emit_rotate_compensation(struct TspdfStream *s, int rot, double cx, double cy);

// Command entry points (argc/argv shifted past the subcommand name)
int cmd_merge(int argc, char **argv);
int cmd_split(int argc, char **argv);
int cmd_rotate(int argc, char **argv);
int cmd_delete(int argc, char **argv);
int cmd_reorder(int argc, char **argv);
int cmd_encrypt(int argc, char **argv);
int cmd_decrypt(int argc, char **argv);
int cmd_metadata(int argc, char **argv);
int cmd_info(int argc, char **argv);
int cmd_watermark(int argc, char **argv);
int cmd_compress(int argc, char **argv);
int cmd_img2pdf(int argc, char **argv);
int cmd_qrcode(int argc, char **argv);
int cmd_md2pdf(int argc, char **argv);
int cmd_serve(int argc, char **argv);
int cmd_text(int argc, char **argv);
int cmd_pagenum(int argc, char **argv);
int cmd_form(int argc, char **argv);
int cmd_attach(int argc, char **argv);
int cmd_stamp(int argc, char **argv);
int cmd_nup(int argc, char **argv);

#endif
