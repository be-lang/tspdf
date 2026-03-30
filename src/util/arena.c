#include "arena.h"
#include <stdlib.h>
#include <string.h>

static TspdfArenaBlock *block_create(size_t capacity) {
    TspdfArenaBlock *block = (TspdfArenaBlock *)malloc(sizeof(TspdfArenaBlock));
    if (!block) return NULL;
    block->data = (uint8_t *)malloc(capacity);
    if (!block->data) {
        free(block);
        return NULL;
    }
    block->capacity = capacity;
    block->offset = 0;
    block->next = NULL;
    return block;
}

TspdfArena tspdf_arena_create(size_t capacity) {
    TspdfArena arena = {0};
    TspdfArenaBlock *block = block_create(capacity);
    if (block) {
        arena.first = block;
        arena.current = block;
        arena.total_allocated = capacity;
    }
    return arena;
}

void tspdf_arena_destroy(TspdfArena *arena) {
    TspdfArenaBlock *block = arena->first;
    while (block) {
        TspdfArenaBlock *next = block->next;
        free(block->data);
        free(block);
        block = next;
    }
    arena->first = NULL;
    arena->current = NULL;
    arena->total_allocated = 0;
}

void *tspdf_arena_alloc(TspdfArena *arena, size_t size) {
    // Align to 8 bytes
    size_t aligned = (size + 7) & ~(size_t)7;

    // Fast path: fits in current block
    TspdfArenaBlock *cur = arena->current;
    if (cur && cur->offset + aligned <= cur->capacity) {
        void *ptr = cur->data + cur->offset;
        cur->offset += aligned;
        return ptr;
    }

    // Slow path: allocate a new block
    size_t new_cap = cur ? cur->capacity * 2 : aligned;
    if (new_cap < aligned) new_cap = aligned;

    TspdfArenaBlock *block = block_create(new_cap);
    if (!block) return NULL;

    // Chain the new block
    if (cur) {
        cur->next = block;
    } else {
        arena->first = block;
    }
    arena->current = block;
    arena->total_allocated += new_cap;

    void *ptr = block->data + block->offset;
    block->offset += aligned;
    return ptr;
}

void *tspdf_arena_alloc_zero(TspdfArena *arena, size_t size) {
    void *ptr = tspdf_arena_alloc(arena, size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void tspdf_arena_reset(TspdfArena *arena) {
    TspdfArenaBlock *block = arena->first;
    while (block) {
        block->offset = 0;
        block = block->next;
    }
    arena->current = arena->first;
}

size_t tspdf_arena_remaining(const TspdfArena *arena) {
    if (!arena->current) return 0;
    return arena->current->capacity - arena->current->offset;
}
