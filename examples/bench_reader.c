#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../include/tspdf.h"
#include "../include/tspdf_overlay.h"
#include "../src/reader/tspr_internal.h"

static double time_ms(struct timespec *start, struct timespec *end) {
    return (end->tv_sec - start->tv_sec) * 1000.0 +
           (end->tv_nsec - start->tv_nsec) / 1000000.0;
}

// Generate a test PDF with N pages
static void generate_pdf(const char *path, int pages) {
    TspdfWriter *doc = tspdf_writer_create();
    const char *font = tspdf_writer_add_builtin_font(doc, "Helvetica");
    for (int i = 0; i < pages; i++) {
        TspdfStream *page = tspdf_writer_add_page(doc);
        tspdf_stream_set_font(page, font, 24.0);
        tspdf_stream_move_to(page, 72, 400);
        char text[64];
        snprintf(text, sizeof(text), "Page %d of %d", i + 1, pages);
        tspdf_stream_show_text(page, text);
        // Add some content to make pages non-trivial
        for (int j = 0; j < 10; j++) {
            tspdf_stream_set_font(page, font, 12.0);
            tspdf_stream_move_to(page, 72, 350 - j * 20);
            tspdf_stream_show_text(page, "Lorem ipsum dolor sit amet, consectetur adipiscing elit.");
        }
    }
    tspdf_writer_save(doc, path);
    tspdf_writer_destroy(doc);
}

#define ITERATIONS 100
#define WARM_UP 5

static void bench_open(const char *label, const uint8_t *data, size_t len) {
    struct timespec start, end;
    TspdfError err;

    // Warm up
    for (int i = 0; i < WARM_UP; i++) {
        TspdfReader *doc = tspdf_reader_open(data, len, &err);
        tspdf_reader_destroy(doc);
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        TspdfReader *doc = tspdf_reader_open(data, len, &err);
        tspdf_reader_destroy(doc);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("  %-35s %8.2f ms  (%d iterations, %.3f ms/op)\n",
           label, time_ms(&start, &end), ITERATIONS,
           time_ms(&start, &end) / ITERATIONS);
}

static void bench_extract(const uint8_t *data, size_t len, size_t *pages, size_t page_count) {
    struct timespec start, end;
    TspdfError err;

    for (int i = 0; i < WARM_UP; i++) {
        TspdfReader *doc = tspdf_reader_open(data, len, &err);
        TspdfReader *ext = tspdf_reader_extract(doc, pages, page_count, &err);
        tspdf_reader_destroy(ext);
        tspdf_reader_destroy(doc);
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        TspdfReader *doc = tspdf_reader_open(data, len, &err);
        TspdfReader *ext = tspdf_reader_extract(doc, pages, page_count, &err);
        tspdf_reader_destroy(ext);
        tspdf_reader_destroy(doc);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    char label[64];
    snprintf(label, sizeof(label), "Extract %zu pages", page_count);
    printf("  %-35s %8.2f ms  (%d iterations, %.3f ms/op)\n",
           label, time_ms(&start, &end), ITERATIONS,
           time_ms(&start, &end) / ITERATIONS);
}

static void bench_save(const char *label, const uint8_t *data, size_t len) {
    struct timespec start, end;
    TspdfError err;

    // Open fresh each iteration (save may mutate internal state)
    for (int i = 0; i < WARM_UP; i++) {
        TspdfReader *doc = tspdf_reader_open(data, len, &err);
        if (!doc) { printf("  [%s: open failed]\n", label); return; }
        uint8_t *out = NULL; size_t out_len = 0;
        tspdf_reader_save_to_memory(doc, &out, &out_len);
        free(out);
        tspdf_reader_destroy(doc);
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        TspdfReader *doc = tspdf_reader_open(data, len, &err);
        uint8_t *out = NULL; size_t out_len = 0;
        tspdf_reader_save_to_memory(doc, &out, &out_len);
        free(out);
        tspdf_reader_destroy(doc);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("  %-35s %8.2f ms  (%d iterations, %.3f ms/op)\n",
           label, time_ms(&start, &end), ITERATIONS,
           time_ms(&start, &end) / ITERATIONS);
}

static void bench_merge(const uint8_t *d1, size_t l1, const uint8_t *d2, size_t l2) {
    struct timespec start, end;
    TspdfError err;

    for (int i = 0; i < WARM_UP; i++) {
        TspdfReader *a = tspdf_reader_open(d1, l1, &err);
        TspdfReader *b = tspdf_reader_open(d2, l2, &err);
        TspdfReader *docs[] = {a, b};
        TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
        tspdf_reader_destroy(merged);
        tspdf_reader_destroy(b);
        tspdf_reader_destroy(a);
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        TspdfReader *a = tspdf_reader_open(d1, l1, &err);
        TspdfReader *b = tspdf_reader_open(d2, l2, &err);
        TspdfReader *docs[] = {a, b};
        TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
        tspdf_reader_destroy(merged);
        tspdf_reader_destroy(b);
        tspdf_reader_destroy(a);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("  %-35s %8.2f ms  (%d iterations, %.3f ms/op)\n",
           "Merge two documents", time_ms(&start, &end), ITERATIONS,
           time_ms(&start, &end) / ITERATIONS);
}

static void bench_encrypt(const char *label, const uint8_t *data, size_t len) {
    struct timespec start, end;
    TspdfError err;

    for (int i = 0; i < WARM_UP; i++) {
        TspdfReader *doc = tspdf_reader_open(data, len, &err);
        if (!doc) { printf("  [%s: open failed]\n", label); return; }
        uint8_t *out = NULL; size_t out_len = 0;
        tspdf_reader_save_to_memory_encrypted(doc, &out, &out_len,
            "user", "owner", 0xFFFFFFFC, 128);
        free(out);
        tspdf_reader_destroy(doc);
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        TspdfReader *doc = tspdf_reader_open(data, len, &err);
        uint8_t *out = NULL; size_t out_len = 0;
        tspdf_reader_save_to_memory_encrypted(doc, &out, &out_len,
            "user", "owner", 0xFFFFFFFC, 128);
        free(out);
        tspdf_reader_destroy(doc);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("  %-35s %8.2f ms  (%d iterations, %.3f ms/op)\n",
           label, time_ms(&start, &end), ITERATIONS,
           time_ms(&start, &end) / ITERATIONS);
}

static void bench_overlay(const uint8_t *data, size_t len) {
    struct timespec start, end;
    TspdfError err;

    for (int i = 0; i < WARM_UP; i++) {
        TspdfReader *doc = tspdf_reader_open(data, len, &err);
        if (!doc) { printf("  [overlay warm-up: open failed]\n"); return; }
        TspdfWriter *res = tspdf_writer_create();
        const char *font = tspdf_writer_add_builtin_font(res, "Helvetica");
        TspdfStream *overlay = tspdf_page_begin_content(doc, 0);
        if (!overlay) { tspdf_writer_destroy(res); tspdf_reader_destroy(doc); printf("  [overlay: begin_content failed]\n"); return; }
        tspdf_stream_set_font(overlay, font, 36.0);
        tspdf_stream_move_to(overlay, 72, 200);
        tspdf_stream_show_text(overlay, "WATERMARK");
        tspdf_page_end_content(doc, 0, overlay, res);
        tspdf_writer_destroy(res);
        tspdf_reader_destroy(doc);
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        TspdfReader *doc = tspdf_reader_open(data, len, &err);
        TspdfWriter *res = tspdf_writer_create();
        const char *font = tspdf_writer_add_builtin_font(res, "Helvetica");
        TspdfStream *overlay = tspdf_page_begin_content(doc, 0);
        tspdf_stream_set_font(overlay, font, 36.0);
        tspdf_stream_move_to(overlay, 72, 200);
        tspdf_stream_show_text(overlay, "WATERMARK");
        tspdf_page_end_content(doc, 0, overlay, res);
        tspdf_writer_destroy(res);
        tspdf_reader_destroy(doc);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("  %-35s %8.2f ms  (%d iterations, %.3f ms/op)\n",
           "Open + overlay + close", time_ms(&start, &end), ITERATIONS,
           time_ms(&start, &end) / ITERATIONS);
}

int main(void) {
    printf("=== tspr Performance Benchmark ===\n\n");

    // Generate test PDFs
    printf("Generating test PDFs...\n");
    generate_pdf("/tmp/bench_10.pdf", 10);
    generate_pdf("/tmp/bench_50.pdf", 50);
    generate_pdf("/tmp/bench_100.pdf", 100);

    // Load into memory
    FILE *f;
    uint8_t *d10, *d50, *d100;
    size_t l10, l50, l100;

    f = fopen("/tmp/bench_10.pdf", "rb"); fseek(f, 0, SEEK_END); l10 = ftell(f); fseek(f, 0, SEEK_SET);
    d10 = malloc(l10); fread(d10, 1, l10, f); fclose(f);

    f = fopen("/tmp/bench_50.pdf", "rb"); fseek(f, 0, SEEK_END); l50 = ftell(f); fseek(f, 0, SEEK_SET);
    d50 = malloc(l50); fread(d50, 1, l50, f); fclose(f);

    f = fopen("/tmp/bench_100.pdf", "rb"); fseek(f, 0, SEEK_END); l100 = ftell(f); fseek(f, 0, SEEK_SET);
    d100 = malloc(l100); fread(d100, 1, l100, f); fclose(f);

    printf("  10-page PDF: %zu bytes\n", l10);
    printf("  50-page PDF: %zu bytes\n", l50);
    printf("  100-page PDF: %zu bytes\n", l100);

    // Binary size
    printf("\n--- Binary TspdfSize ---\n");
    printf("  (check with: ls -lh build/bench_reader)\n");

    // Open benchmarks
    printf("\n--- Open (parse xref + page tree) ---\n");
    bench_open("Open 10-page PDF", d10, l10);
    bench_open("Open 50-page PDF", d50, l50);
    bench_open("Open 100-page PDF", d100, l100);

    // Extract benchmarks
    printf("\n--- Extract Pages ---\n");
    size_t pages_1[] = {0};
    size_t pages_5[] = {0, 10, 20, 30, 40};
    bench_extract(d50, l50, pages_1, 1);
    bench_extract(d50, l50, pages_5, 5);

    // Save benchmarks
    printf("\n--- Open + Save (round-trip to memory) ---\n");
    bench_save("Round-trip 10-page", d10, l10);
    bench_save("Round-trip 50-page", d50, l50);
    bench_save("Round-trip 100-page", d100, l100);

    // Merge benchmarks
    printf("\n--- Merge ---\n");
    bench_merge(d10, l10, d50, l50);

    // Encrypt benchmarks
    printf("\n--- Encrypt (AES-128) ---\n");
    bench_encrypt("Encrypt 10-page", d10, l10);
    bench_encrypt("Encrypt 50-page", d50, l50);

    // Overlay benchmarks
    printf("\n--- Content Overlay ---\n");
    bench_overlay(d10, l10);
    bench_overlay(d50, l50);

    // File sizes
    printf("\n--- Output Sizes ---\n");
    {
        TspdfError err;
        TspdfReader *doc = tspdf_reader_open(d100, l100, &err);
        uint8_t *out = NULL; size_t out_len = 0;
        tspdf_reader_save_to_memory(doc, &out, &out_len);
        printf("  100-page input: %zu bytes, output: %zu bytes (%.1f%%)\n",
               l100, out_len, 100.0 * out_len / l100);
        free(out);
        tspdf_reader_destroy(doc);
    }

    free(d10); free(d50); free(d100);
    remove("/tmp/bench_10.pdf");
    remove("/tmp/bench_50.pdf");
    remove("/tmp/bench_100.pdf");

    printf("\nDone.\n");
    return 0;
}
