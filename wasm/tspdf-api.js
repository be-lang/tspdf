// Thin JS wrapper over the wasm C ABI (wasm/shim.c). Shared by the node test
// harness and the browser backend. `Module` is the object resolved from the
// emscripten factory (createTspdf()).
//
// All byte results are copied out of the wasm heap into fresh Uint8Arrays and
// the wasm-side buffers freed, so callers never hold heap views that a later
// memory growth would detach.

export function makeTspdf(Module) {
  const lastError = () => Module.UTF8ToString(Module._tspdf_wasm_last_error());

  function fail() {
    throw new Error(lastError() || 'tspdf wasm call failed');
  }

  function allocBytes(bytes) {
    const p = Module._malloc(bytes.length);
    if (!p) throw new Error('wasm out of memory');
    Module.HEAPU8.set(bytes, p);
    return p;
  }

  function allocCString(s) {
    if (s === null || s === undefined) return 0;
    const n = Module.lengthBytesUTF8(s) + 1;
    const p = Module._malloc(n);
    if (!p) throw new Error('wasm out of memory');
    Module.stringToUTF8(s, p, n);
    return p;
  }

  function allocU32Array(values) {
    const p = Module._malloc(Math.max(values.length, 1) * 4);
    if (!p) throw new Error('wasm out of memory');
    for (let i = 0; i < values.length; i++) {
      Module.HEAPU32[(p >> 2) + i] = values[i];
    }
    return p;
  }

  // Run fn(lenPtr) -> result pointer; copy out the bytes and free them.
  function takeResult(fn) {
    const lenPtr = Module._malloc(4);
    if (!lenPtr) throw new Error('wasm out of memory');
    let ptr = 0;
    try {
      ptr = fn(lenPtr);
      if (!ptr) fail();
      const len = Module.HEAPU32[lenPtr >> 2];
      return new Uint8Array(Module.HEAPU8.buffer, ptr, len).slice();
    } finally {
      if (ptr) Module._tspdf_wasm_free_result(ptr);
      Module._free(lenPtr);
    }
  }

  return {
    version() {
      return Module.UTF8ToString(Module._tspdf_wasm_version());
    },

    // bytes: Uint8Array; password: string or null. Returns a handle.
    open(bytes, password = null) {
      const passPtr = allocCString(password);
      try {
        // The file buffer is allocated last so nothing can throw between its
        // allocation and the open call, which takes ownership of it (and
        // frees it on failure).
        const dataPtr = allocBytes(bytes);
        const h = Module._tspdf_wasm_open(dataPtr, bytes.length, passPtr);
        if (!h) fail();
        return h;
      } finally {
        Module._free(passPtr);
      }
    },

    close(h) {
      Module._tspdf_wasm_close(h);
    },

    pageCount(h) {
      const n = Module._tspdf_wasm_page_count(h);
      if (n < 0) fail();
      return n;
    },

    save(h) {
      return takeResult((lp) => Module._tspdf_wasm_save(h, lp));
    },

    merge(handles) {
      const arr = allocU32Array(handles);
      try {
        return takeResult((lp) =>
          Module._tspdf_wasm_merge(arr, handles.length, lp));
      } finally {
        Module._free(arr);
      }
    },

    // pages: zero-based page indices.
    extract(h, pages) {
      const arr = allocU32Array(pages);
      try {
        return takeResult((lp) =>
          Module._tspdf_wasm_extract(h, arr, pages.length, lp));
      } finally {
        Module._free(arr);
      }
    },

    deletePages(h, pages) {
      const arr = allocU32Array(pages);
      try {
        return takeResult((lp) =>
          Module._tspdf_wasm_delete(h, arr, pages.length, lp));
      } finally {
        Module._free(arr);
      }
    },

    reorder(h, order) {
      const arr = allocU32Array(order);
      try {
        return takeResult((lp) =>
          Module._tspdf_wasm_reorder(h, arr, order.length, lp));
      } finally {
        Module._free(arr);
      }
    },

    // pages: zero-based indices, or null/[] for all pages.
    rotate(h, pages, angle) {
      const list = pages || [];
      const arr = allocU32Array(list);
      try {
        return takeResult((lp) =>
          Module._tspdf_wasm_rotate(h, arr, list.length, angle, lp));
      } finally {
        Module._free(arr);
      }
    },

    compress(h, { stripUnused = true, recompress = true } = {}) {
      return takeResult((lp) =>
        Module._tspdf_wasm_compress(h, stripUnused ? 1 : 0, recompress ? 1 : 0, lp));
    },

    encrypt(h, userPass, ownerPass, bits = 128) {
      const up = allocCString(userPass ?? '');
      const op = allocCString(ownerPass ?? userPass ?? '');
      try {
        return takeResult((lp) =>
          Module._tspdf_wasm_encrypt(h, up, op, bits, lp));
      } finally {
        Module._free(up);
        Module._free(op);
      }
    },

    watermark(h, text, { fontSize = 48, opacity = 0.3 } = {}) {
      const tp = allocCString(text ?? 'DRAFT');
      try {
        return takeResult((lp) =>
          Module._tspdf_wasm_watermark_text(h, tp, fontSize, opacity, lp));
      } finally {
        Module._free(tp);
      }
    },

    // fields: {title, author, subject, keywords}; missing/null = unchanged.
    setMetadata(h, fields) {
      const ptrs = ['title', 'author', 'subject', 'keywords'].map((k) =>
        allocCString(fields[k] ?? null));
      try {
        return takeResult((lp) =>
          Module._tspdf_wasm_set_metadata(h, ptrs[0], ptrs[1], ptrs[2], ptrs[3], lp));
      } finally {
        ptrs.forEach((p) => Module._free(p));
      }
    },

    getMetadata(h) {
      const ptr = Module._tspdf_wasm_metadata_json(h);
      if (!ptr) fail();
      const json = Module.UTF8ToString(ptr);
      Module._tspdf_wasm_free_result(ptr);
      return JSON.parse(json);
    },
  };
}
