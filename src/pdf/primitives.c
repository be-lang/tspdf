#include "primitives.h"

/*
 * Canonical PDF-syntax primitive encoders.
 * Both the reader serializer and the raw writer delegate here.
 * See primitives.h for the spec references.
 */

void tspdf_pdf_encode_string(TspdfBuffer *buf, const uint8_t *data, size_t len) {
    tspdf_buffer_append_byte(buf, '(');
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        switch (c) {
            case '(':  tspdf_buffer_append_str(buf, "\\("); break;
            case ')':  tspdf_buffer_append_str(buf, "\\)"); break;
            case '\\': tspdf_buffer_append_str(buf, "\\\\"); break;
            case '\n': tspdf_buffer_append_str(buf, "\\n"); break;
            case '\r': tspdf_buffer_append_str(buf, "\\r"); break;
            case '\t': tspdf_buffer_append_str(buf, "\\t"); break;
            case '\b': tspdf_buffer_append_str(buf, "\\b"); break;
            case '\f': tspdf_buffer_append_str(buf, "\\f"); break;
            default:
                if (c < 32 || c > 126) {
                    tspdf_buffer_printf(buf, "\\%03o", c);
                } else {
                    tspdf_buffer_append_byte(buf, c);
                }
                break;
        }
    }
    tspdf_buffer_append_byte(buf, ')');
}

void tspdf_pdf_encode_name(TspdfBuffer *buf, const uint8_t *data, size_t len) {
    tspdf_buffer_append_byte(buf, '/');
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        /* Escape non-regular characters per PDF spec §7.3.5:
         * any byte outside [33..126], or one of the delimiter/special chars */
        if (c < 33 || c > 126 ||
            c == '#' || c == '/' || c == '(' || c == ')' ||
            c == '<' || c == '>' || c == '[' || c == ']' ||
            c == '{' || c == '}' || c == '%') {
            tspdf_buffer_printf(buf, "#%02X", c);
        } else {
            tspdf_buffer_append_byte(buf, c);
        }
    }
}
