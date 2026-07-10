// In-browser backend for the static tspdf demo. Implements the same tool
// operations as the local server's /api/ endpoints, but entirely client-side
// via the wasm module. Installed as window.TSPDF_BACKEND (the seam in
// web/static/app.js), and also patches window.fetch for the one template
// (metadata) that calls fetch('/api/metadata-view') directly.

import createTspdf from './tspdf.js';
import { makeTspdf } from './tspdf-api.js';

const modulePromise = createTspdf();

async function fileBytes(file) {
  return new Uint8Array(await file.arrayBuffer());
}

// "1-3,5" (1-based, inclusive ranges) -> [0,1,2,4]. Returns null for
// empty/"all". Throws on malformed input.
function parsePageRange(str) {
  const s = (str || '').trim();
  if (!s || s.toLowerCase() === 'all') return null;
  const pages = [];
  for (const tok of s.split(',')) {
    const part = tok.trim();
    if (!part) continue;
    const m = part.match(/^(\d+)(?:\s*-\s*(\d+))?$/);
    if (!m) throw new Error(`Invalid page range: "${part}"`);
    const lo = parseInt(m[1], 10);
    const hi = m[2] ? parseInt(m[2], 10) : lo;
    if (lo < 1 || hi < lo) throw new Error(`Invalid page range: "${part}"`);
    for (let p = lo; p <= hi; p++) pages.push(p - 1);
  }
  if (!pages.length) throw new Error('Empty page range');
  return pages;
}

// Open the single uploaded PDF, run fn(t, handle), close the handle.
async function withDoc(t, files, fn, password = null) {
  const file = files.pdf_file;
  if (!file) throw new Error('Missing pdf_file');
  const h = t.open(await fileBytes(file), password);
  try {
    return await fn(h);
  } finally {
    t.close(h);
  }
}

const handlers = {
  async merge(t, config, files) {
    // merge.html names uploads pdf_file_0..N; sort numerically so the merge
    // preserves submission order past 10 files (lexicographic would put
    // pdf_file_10 before pdf_file_2).
    const uploads = Object.keys(files)
      .filter((k) => k.startsWith('pdf_file'))
      .sort((a, b) => parseInt(a.slice(9), 10) - parseInt(b.slice(9), 10))
      .map((k) => files[k]);
    if (uploads.length < 2) throw new Error('Need at least 2 PDF files to merge');
    const handles = [];
    try {
      for (const f of uploads) handles.push(t.open(await fileBytes(f)));
      return t.merge(handles);
    } finally {
      handles.forEach((h) => t.close(h));
    }
  },

  async split(t, config, files) {
    const pages = parsePageRange(config.pages);
    if (!pages) throw new Error('Missing pages in config');
    return withDoc(t, files, (h) => t.extract(h, pages));
  },

  async 'delete-pages'(t, config, files) {
    const pages = parsePageRange(config.pages);
    if (!pages) throw new Error('Missing pages in config');
    return withDoc(t, files, (h) => t.deletePages(h, pages));
  },

  async rotate(t, config, files) {
    const pages = parsePageRange(config.pages); // null = all
    const angle = parseInt(config.angle, 10) || 90;
    return withDoc(t, files, (h) => t.rotate(h, pages, angle));
  },

  async reorder(t, config, files) {
    const order = (config.order || '').split(',')
      .map((s) => parseInt(s.trim(), 10))
      .filter((n) => !Number.isNaN(n))
      .map((n) => n - 1);
    if (!order.length) throw new Error('Missing order in config');
    if (order.some((n) => n < 0)) throw new Error('Invalid page number in order');
    return withDoc(t, files, (h) => t.reorder(h, order));
  },

  async compress(t, config, files) {
    return withDoc(t, files, (h) => t.compress(h, {
      stripUnused: config.strip_unused !== '0',
      recompress: config.recompress !== '0',
    }));
  },

  async unlock(t, config, files) {
    // A plain save preserves the source encryption; unlock must opt out.
    return withDoc(t, files, (h) => t.saveDecrypted(h), config.password || '');
  },

  async 'password-protect'(t, config, files) {
    if (!config.password) throw new Error('Missing password in config');
    const bits = parseInt(config.bits, 10) === 256 ? 256 : 128;
    const owner = config.owner_password || config.password;
    return withDoc(t, files, (h) => t.encrypt(h, config.password, owner, bits));
  },

  async metadata(t, config, files) {
    return withDoc(t, files, (h) => t.setMetadata(h, {
      title: config.title ?? null,
      author: config.author ?? null,
      subject: config.subject ?? null,
      keywords: config.keywords ?? null,
    }));
  },

  async 'metadata-view'(t, config, files) {
    return withDoc(t, files, (h) => t.getMetadata(h));
  },

  async 'watermark-existing'(t, config, files) {
    const text = config.watermark_text || config.text || 'DRAFT';
    const fontSize = parseFloat(config.font_size) || 48;
    const opacity = parseFloat(config.opacity) || 0.3;
    return withDoc(t, files, (h) => t.watermark(h, text, { fontSize, opacity }));
  },
};

window.TSPDF_BACKEND = {
  supports: Object.keys(handlers),

  // Mirrors the server contract: resolves to a Response whose body is the
  // produced PDF (or JSON for metadata-view); errors become non-ok Responses
  // with a plain-text message, which app.js turns into an actionable tip.
  async run(toolId, config, files) {
    const handler = handlers[toolId];
    if (!handler) {
      return new Response('This tool is not available in the browser demo.', { status: 400 });
    }
    try {
      const Module = await modulePromise;
      const t = makeTspdf(Module);
      const result = await handler(t, config || {}, files || {});
      if (result instanceof Uint8Array) {
        return new Response(new Blob([result], { type: 'application/pdf' }), { status: 200 });
      }
      return new Response(JSON.stringify(result), {
        status: 200,
        headers: { 'Content-Type': 'application/json' },
      });
    } catch (err) {
      return new Response(String((err && err.message) || err), { status: 400 });
    }
  },
};

// metadata.html posts to fetch('/api/metadata-view') directly rather than
// through setupTool, so route /api/ fetches into the backend as well.
const origFetch = window.fetch.bind(window);
window.fetch = async function (input, init) {
  const url = typeof input === 'string' ? input : (input && input.url) || '';
  const m = url.match(/^\/api\/([a-z0-9-]+)$/);
  if (!m) return origFetch(input, init);
  const config = {};
  const files = {};
  const body = init && init.body;
  if (body instanceof FormData) {
    for (const [key, value] of body.entries()) {
      if (key === 'config') {
        try { Object.assign(config, JSON.parse(value)); } catch { /* ignore */ }
      } else if (value instanceof Blob) {
        files[key] = value;
      }
    }
  }
  return window.TSPDF_BACKEND.run(m[1], config, files);
};
