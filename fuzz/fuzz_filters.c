// libFuzzer harness for the stream-filter decode chain (src/filters).
//
// Build:  make fuzz
// Run:    ./build/fuzz/fuzz_filters fuzz/corpus/filters
//
// tspdf_filter_decode() decodes every /Filter application in every PDF stream
// (ASCIIHex / ASCII85 / RunLength / LZW / Flate + predictors). A wrong bound
// check here reads or writes out of bounds on attacker-controlled bytes. This
// harness consumes the first input byte as a filter-name selector and feeds the
// remaining bytes (params-less) through the single-filter entry point, freeing
// any output. It asserts nothing about the decoded value — the oracle is
// ASan/UBSan: any out-of-bounds access, leak, or UB aborts the run.

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "../src/filters/filters.h"

// Both the unabbreviated names and their PDF abbreviations, so the fuzzer
// exercises every dispatch branch including the abbreviation aliases and the
// unsupported-filter path.
static const char *const kFilterNames[] = {
    "ASCIIHexDecode", "AHx",
    "ASCII85Decode",  "A85",
    "RunLengthDecode", "RL",
    "LZWDecode",      "LZW",
    "FlateDecode",    "Fl",
    "DCTDecode",      // unsupported: must return TSPDF_ERR_UNSUPPORTED, no output
};
enum { kFilterCount = sizeof(kFilterNames) / sizeof(kFilterNames[0]) };

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) {
        return 0;
    }

    const char *filter_name = kFilterNames[data[0] % kFilterCount];
    const uint8_t *payload = data + 1;
    size_t payload_len = size - 1;

    size_t out_len = 0;
    TspdfError err = TSPDF_OK;
    // params-less: NULL DecodeParms dict (predictor 1, EarlyChange default).
    uint8_t *out = tspdf_filter_decode(filter_name, payload, payload_len,
                                       NULL, &out_len, &err);
    if (out) {
        free(out);
    }
    return 0;
}
