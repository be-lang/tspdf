#ifndef TSPDF_H
#define TSPDF_H

// tspdf — PDF toolkit in pure C. Zero dependencies.

// Error handling
#include "../src/tspdf_error.h"

// PDF generation (writer)
#include "../src/pdf/tspdf_writer.h"
#include "../src/pdf/pdf_stream.h"

// PDF reading and manipulation (reader)
#include "../src/reader/tspr.h"

// Layout engine
#include "../src/layout/layout.h"

// Memory management
#include "../src/util/arena.h"

#endif
