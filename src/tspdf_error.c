#include "tspdf_error.h"

const char *tspdf_error_string(TspdfError err) {
    switch (err) {
        case TSPDF_OK:              return "success";
        case TSPDF_ERR_ALLOC:       return "memory allocation failed";
        case TSPDF_ERR_IO:          return "file I/O error";
        case TSPDF_ERR_FONT_PARSE:  return "font parsing failed";
        case TSPDF_ERR_FONT_LIMIT:  return "maximum font count exceeded";
        case TSPDF_ERR_PAGE_LIMIT:  return "maximum page count exceeded";
        case TSPDF_ERR_IMAGE_LIMIT: return "maximum image count exceeded";
        case TSPDF_ERR_IMAGE_PARSE: return "image parsing failed";
        case TSPDF_ERR_INVALID_ARG: return "invalid argument";
        case TSPDF_ERR_OVERFLOW:    return "overflow";
        case TSPDF_ERR_ENCODING:    return "invalid UTF-8 encoding";
        case TSPDF_ERR_INVALID_PDF: return "not a valid PDF";
        case TSPDF_ERR_PARSE:       return "PDF parsing failed";
        case TSPDF_ERR_XREF:        return "cross-reference table error";
        case TSPDF_ERR_UNSUPPORTED: return "unsupported PDF feature";
        case TSPDF_ERR_ENCRYPTED:   return "PDF is encrypted, password required";
        case TSPDF_ERR_BAD_PASSWORD: return "wrong password";
    }
    return "unknown error";
}
