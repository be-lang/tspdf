#ifndef TSPDF_ARENA_H
#define TSPDF_ARENA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct TspdfArenaBlock {
    uint8_t *data;
    size_t capacity;
    size_t offset;
    struct TspdfArenaBlock *next;
} TspdfArenaBlock;

typedef struct {
    TspdfArenaBlock *first;      // first block (for reset/destroy)
    TspdfArenaBlock *current;    // current block for allocations
    size_t total_allocated; // total bytes across all blocks
} TspdfArena;

TspdfArena tspdf_arena_create(size_t capacity);
void tspdf_arena_destroy(TspdfArena *arena);
void *tspdf_arena_alloc(TspdfArena *arena, size_t size);
void *tspdf_arena_alloc_zero(TspdfArena *arena, size_t size);
void tspdf_arena_reset(TspdfArena *arena);
size_t tspdf_arena_remaining(const TspdfArena *arena);

#endif
