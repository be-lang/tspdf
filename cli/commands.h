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

// Collect positional args (not flags). Returns count.
int collect_positional(int argc, char **argv, const char **out, int out_max);

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

#endif
