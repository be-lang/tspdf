#ifndef TSPDF_READER_CONTENT_WALK_H
#define TSPDF_READER_CONTENT_WALK_H

// Shared content-stream walker (see tspr_content_walk.c). Drives operator
// tokenization, the q/Q + cm graphics-state stack, Form XObject recursion, and
// inline-image skipping; clients receive callbacks for the operators they act
// on and for image Do with the effective CTM.

#include "tspr_internal.h"

// PDF affine matrix (row-vector convention: [a b 0; c d 0; e f 1]).
typedef struct {
    double a, b, c, d, e, f;
} TspdfWalkMat;

extern const TspdfWalkMat TSPDF_WALK_IDENTITY;

// n applied after m (m first). Identical to the clients' old tx_mul/mat_mul.
TspdfWalkMat tspdf_walk_mat_mul(TspdfWalkMat m, TspdfWalkMat n);

// Form XObject recursion cap and operand-stack depth. The op stack is a
// superset of both clients' needs (text used 16, lossy 8; operators consume at
// most 6). The gstate stack starts fixed and grows on the heap to GS_MAX; past
// it, CTM tracking is lost for the rest of the stream (lossy poisons, text
// keeps drawing).
#define TSPDF_WALK_MAX_DEPTH 8
#define TSPDF_WALK_OP_STACK 16
#define TSPDF_WALK_GS_STACK 64
#define TSPDF_WALK_GS_MAX 4096

typedef struct TspdfWalker TspdfWalker;

struct TspdfWalker {
    TspdfReader *doc;
    TspdfParser *rp;       // object resolution parser (doc arena)
    TspdfArena *scratch;   // transient operand parsing
    void *client;          // opaque client context passed to every callback

    int max_depth;         // form recursion cap (usually TSPDF_WALK_MAX_DEPTH)
    size_t gstate_size;    // bytes of client per-gstate blob (0 = none)

    // Load a content/form stream (decrypt + filter chain); malloc'd result the
    // walker frees via load_stream_free. obj_num is the stream's object number
    // (0 for inline/self-contained) for per-object decryption keys.
    uint8_t *(*load_stream)(void *client, TspdfObj *stream, uint32_t obj_num,
                            size_t *out_len);
    void (*load_stream_free)(void *client, uint8_t *bytes);

    // Called for every operator the walker does not handle itself (i.e. not
    // q/Q/cm/Do/BI/true/false/null). Operands are stk[0..ns-1] bottom-to-top;
    // use tspdf_walk_operand / tspdf_walk_operand_num (from_top indexing). ctm
    // and resources are the current effective values; gstate is the client
    // blob. May be NULL.
    void (*op)(void *client, const char *op, TspdfObj **stk, int ns,
               TspdfWalkMat ctm, TspdfObj *resources, void *gstate);

    // Called when `Do` names an Image (or unknown-subtype) XObject, with the
    // effective CTM and whether the CTM is untracked. May be NULL.
    void (*image_do)(void *client, TspdfObj *xo, uint32_t obj_num,
                     TspdfWalkMat ctm, bool ctm_lost);

    // Called just after a Form is entered, on the sub-stream's private gstate
    // blob copy, so the client can reset per-form state (text zeroes its text
    // matrices). May be NULL.
    void (*form_enter)(void *client, void *gstate, TspdfObj *form);

    // Cycle guard, indexed by depth. Sized to max_depth+1.
    const TspdfObj *form_stack[TSPDF_WALK_MAX_DEPTH];
};

// Interpret `data` with the given effective CTM, resources, recursion depth,
// ctm_lost flag, and client gstate blob. Top-level callers pass depth 0,
// ctm_lost false, ctm = identity.
void tspdf_walk_run(TspdfWalker *w, const uint8_t *data, size_t len,
                    TspdfWalkMat ctm, TspdfObj *resources, int depth,
                    bool ctm_lost, void *gstate);

// Resolve a possibly-indirect object (bounds-checked; NULL on out-of-range).
TspdfObj *tspdf_walk_resolve(TspdfWalker *w, TspdfObj *o);

// Operand accessors: from_top 0 is the topmost operand.
TspdfObj *tspdf_walk_operand(TspdfObj **stk, int ns, int from_top);
double tspdf_walk_operand_num(TspdfObj **stk, int ns, int from_top);

#endif
