#define _POSIX_C_SOURCE 199309L
// tspdf benchmark: measures time and memory for PDF generation tasks
//
// Build: make bench
// Run:   ./build/bench
//
// Compares against libharu if available:
//   gcc -DWITH_LIBHARU -o build/bench_haru examples/benchmark.c -lhpdf -lz -lm
//   ./build/bench_haru

#include "../include/tspdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef WITH_LIBHARU
#include <hpdf.h>
#endif

// --- Timing ---
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// --- Memory tracking (Linux /proc/self/status) ---
static long get_peak_rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    long peak = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmHWM:", 6) == 0) {
            sscanf(line + 6, "%ld", &peak);
            break;
        }
    }
    fclose(f);
    return peak;
}

// ============================================================
// Benchmark 1: Many pages with text
// ============================================================
static void bench_tspdf_text_pages(int num_pages, const char *output_path) {
    TspdfWriter *doc = tspdf_writer_create();
    const char *font = tspdf_writer_add_builtin_font(doc, "Helvetica");

    for (int p = 0; p < num_pages; p++) {
        TspdfStream *s = tspdf_writer_add_page(doc);
        for (int line = 0; line < 50; line++) {
            tspdf_stream_begin_text(s);
            tspdf_stream_set_font(s, font, 10);
            tspdf_stream_text_position(s, 72, 750 - line * 14);
            char buf[128];
            snprintf(buf, sizeof(buf),
                "Page %d, line %d: The quick brown fox jumps over the lazy dog.", p + 1, line + 1);
            tspdf_stream_show_text(s, buf);
            tspdf_stream_end_text(s);
        }
    }

    tspdf_writer_save(doc, output_path);
    tspdf_writer_destroy(doc);
}

// ============================================================
// Benchmark 2: TTF font with Unicode text
// ============================================================
static void bench_tspdf_unicode(int num_pages, const char *output_path) {
    TspdfWriter *doc = tspdf_writer_create();
    const char *font = tspdf_writer_add_ttf_font(doc,
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf");
    if (!font) {
        printf("  [skipped - LiberationSans not found]\n");
        tspdf_writer_destroy(doc);
        return;
    }
    TTF_Font *ttf = tspdf_writer_get_ttf(doc, font);
    TspdfFont *pdf_font = tspdf_writer_get_font(doc, font);

    for (int p = 0; p < num_pages; p++) {
        TspdfStream *s = tspdf_writer_add_page(doc);
        for (int line = 0; line < 50; line++) {
            tspdf_stream_begin_text(s);
            tspdf_stream_set_font(s, font, 10);
            tspdf_stream_text_position(s, 72, 750 - line * 14);
            char buf[256];
            snprintf(buf, sizeof(buf),
                "Page %d: Fran\xC3\xA7" "ais \xC3\xA9l\xC3\xA8ve, Deutsch \xC3\xBC" "ber Gr\xC3\xBC\xC3\x9F" "e, Espa\xC3\xB1ol \xC2\xA1Hola! \xE2\x82\xAC%d",
                p + 1, line);
            tspdf_stream_show_text_utf8(s, buf, ttf, pdf_font);
            tspdf_stream_end_text(s);
        }
    }

    tspdf_writer_save(doc, output_path);
    tspdf_writer_destroy(doc);
}

// ============================================================
// Benchmark 3: Layout engine with boxes and text
// ============================================================

static TspdfWriter *g_bench_doc = NULL;

static double bench_measure_cb(const char *font_name, double font_size,
                                const char *text, void *userdata) {
    (void)userdata;
    return tspdf_writer_measure_text(g_bench_doc, font_name, font_size, text);
}

static double bench_line_height_cb(const char *font_name, double font_size,
                                    void *userdata) {
    (void)userdata;
    TTF_Font *ttf = tspdf_writer_get_ttf(g_bench_doc, font_name);
    if (ttf) return ttf_get_line_height(ttf, font_size);
    const TspdfBase14Metrics *b14 = tspdf_writer_get_base14(g_bench_doc, font_name);
    if (b14) return tspdf_base14_line_height(b14, font_size);
    return font_size * 1.2;
}

static void bench_tspdf_layout(int num_rows, const char *output_path) {
    TspdfWriter *doc = tspdf_writer_create();
    g_bench_doc = doc;
    const char *font = tspdf_writer_add_builtin_font(doc, "Helvetica");
    const char *bold = tspdf_writer_add_builtin_font(doc, "Helvetica-Bold");

    TspdfArena arena = tspdf_arena_create(64 * 1024 * 1024);
    TspdfLayout ctx = tspdf_layout_create(&arena);
    ctx.measure_text = bench_measure_cb;
    ctx.font_line_height = bench_line_height_cb;
    ctx.doc = doc;

    // Build pages in batches of TSPDF_LAYOUT_MAX_CHILDREN-1 rows
    // (one slot reserved for the repeating header)
    int rows_per_batch = TSPDF_LAYOUT_MAX_CHILDREN - 1;
    int total_rows_done = 0;

    while (total_rows_done < num_rows) {
        int batch = num_rows - total_rows_done;
        if (batch > rows_per_batch) batch = rows_per_batch;

        TspdfNode *root = tspdf_layout_box(&ctx);
        root->width = tspdf_size_fixed(TSPDF_PAGE_A4_WIDTH);
        root->height = tspdf_size_fit();
        root->direction = TSPDF_DIR_COLUMN;
        root->padding = tspdf_padding_all(30);
        root->gap = 0;

        // Header
        TspdfNode *hdr = tspdf_layout_box(&ctx);
        hdr->width = tspdf_size_grow();
        hdr->height = tspdf_size_fixed(30);
        hdr->direction = TSPDF_DIR_ROW;
        hdr->align_y = TSPDF_ALIGN_CENTER;
        {
            TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, hdr);
            s->has_background = true;
            s->background = tspdf_color_from_u8(30, 30, 46);
        }
        hdr->repeat_mode = TSPDF_REPEAT_ALL;
        tspdf_layout_add_child(root, hdr);

        const char *cols[] = {"#", "Name", "Value", "Status"};
        for (int c = 0; c < 4; c++) {
            TspdfNode *cell = tspdf_layout_text(&ctx, cols[c], bold, 10);
            cell->text.color = tspdf_color_rgb(1, 1, 1);
            cell->width = tspdf_size_percent(c == 0 ? 10 : 30);
            cell->padding = tspdf_padding_xy(8, 4);
            tspdf_layout_add_child(hdr, cell);
        }

        // Rows for this batch
        for (int r = 0; r < batch; r++) {
            int row_num = total_rows_done + r;
            TspdfNode *row = tspdf_layout_box(&ctx);
            row->width = tspdf_size_grow();
            row->height = tspdf_size_fixed(24);
            row->direction = TSPDF_DIR_ROW;
            row->align_y = TSPDF_ALIGN_CENTER;
            if (row_num % 2) {
                {
                    TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, row);
                    s->has_background = true;
                    s->background = tspdf_color_from_u8(245, 245, 250);
                }
            }
            tspdf_layout_add_child(root, row);

            char buf[64];
            snprintf(buf, sizeof(buf), "%d", row_num + 1);
            TspdfNode *c0 = tspdf_layout_text(&ctx, buf, font, 9);
            c0->width = tspdf_size_percent(10); c0->padding = tspdf_padding_xy(8, 4);
            tspdf_layout_add_child(row, c0);

            snprintf(buf, sizeof(buf), "Item %d", row_num + 1);
            TspdfNode *c1 = tspdf_layout_text(&ctx, buf, font, 9);
            c1->width = tspdf_size_percent(30); c1->padding = tspdf_padding_xy(8, 4);
            tspdf_layout_add_child(row, c1);

            snprintf(buf, sizeof(buf), "%d.%02d", (row_num * 17) % 1000, (row_num * 31) % 100);
            TspdfNode *c2 = tspdf_layout_text(&ctx, buf, font, 9);
            c2->width = tspdf_size_percent(30); c2->padding = tspdf_padding_xy(8, 4);
            tspdf_layout_add_child(row, c2);

            TspdfNode *c3 = tspdf_layout_text(&ctx, row_num % 3 == 0 ? "Active" : row_num % 3 == 1 ? "Pending" : "Done", font, 9);
            c3->width = tspdf_size_percent(30); c3->padding = tspdf_padding_xy(8, 4);
            tspdf_layout_add_child(row, c3);
        }

        TspdfPaginationResult pag;
        int pages = tspdf_layout_compute_paginated(&ctx, root, TSPDF_PAGE_A4_WIDTH, TSPDF_PAGE_A4_HEIGHT, &pag);

        for (int p = 0; p < pages; p++) {
            TspdfStream *s = tspdf_writer_add_page(doc);
            tspdf_layout_render_page(&ctx, root, &pag, p, s);
        }

        total_rows_done += batch;
        tspdf_layout_tree_free(root);
    }

    tspdf_writer_save(doc, output_path);
    tspdf_arena_destroy(&arena);
    tspdf_writer_destroy(doc);
}

// ============================================================
// Benchmark runner
// ============================================================

typedef struct {
    const char *name;
    double time_ms;
    long file_bytes;
    long peak_rss_kb;
} BenchResult;

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return sz;
}

int main(void) {
    printf("tspdf benchmark\n");
    printf("==============\n\n");

    BenchResult results[10];
    int n = 0;

    // Bench 1: 100 pages of text (built-in font)
    {
        const char *path = "/tmp/tspdf_bench_text100.pdf";
        double t0 = now_ms();
        bench_tspdf_text_pages(100, path);
        double t1 = now_ms();
        results[n++] = (BenchResult){
            "100 pages, 50 lines each (Helvetica)", t1 - t0, file_size(path), get_peak_rss_kb()
        };
        remove(path);
    }

    // Bench 2: 10 pages of text (built-in font)
    {
        const char *path = "/tmp/tspdf_bench_text10.pdf";
        double t0 = now_ms();
        bench_tspdf_text_pages(10, path);
        double t1 = now_ms();
        results[n++] = (BenchResult){
            "10 pages, 50 lines each (Helvetica)", t1 - t0, file_size(path), get_peak_rss_kb()
        };
        remove(path);
    }

    // Bench 3: 50 pages Unicode text (TTF)
    {
        const char *path = "/tmp/tspdf_bench_unicode50.pdf";
        double t0 = now_ms();
        bench_tspdf_unicode(50, path);
        double t1 = now_ms();
        results[n++] = (BenchResult){
            "50 pages, 50 lines Unicode (LiberationSans TTF)", t1 - t0, file_size(path), get_peak_rss_kb()
        };
        remove(path);
    }

    // Bench 4: Layout engine - 500 row table
    {
        const char *path = "/tmp/tspdf_bench_layout500.pdf";
        double t0 = now_ms();
        bench_tspdf_layout(500, path);
        double t1 = now_ms();
        results[n++] = (BenchResult){
            "500-row paginated table (layout engine)", t1 - t0, file_size(path), get_peak_rss_kb()
        };
        remove(path);
    }

    // Bench 5: Layout engine - 2000 row table
    {
        const char *path = "/tmp/tspdf_bench_layout2000.pdf";
        double t0 = now_ms();
        bench_tspdf_layout(2000, path);
        double t1 = now_ms();
        results[n++] = (BenchResult){
            "2000-row paginated table (layout engine)", t1 - t0, file_size(path), get_peak_rss_kb()
        };
        remove(path);
    }

    // Print results
    printf("%-55s %10s %10s %10s\n", "Benchmark", "Time (ms)", "TspdfSize (KB)", "RSS (KB)");
    printf("%-55s %10s %10s %10s\n", "-------", "---------", "---------", "--------");
    for (int i = 0; i < n; i++) {
        printf("%-55s %10.1f %10.1f %10ld\n",
            results[i].name,
            results[i].time_ms,
            results[i].file_bytes / 1024.0,
            results[i].peak_rss_kb);
    }

    printf("\n");
    return 0;
}
