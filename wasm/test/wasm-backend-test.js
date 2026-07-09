// Node harness for the demo's browser backend (wasm/demo/wasm-backend.js).
// run_wasm_test.sh copies the backend next to the module in <outdir> — the
// same layout the demo site ships — so its relative imports resolve, and
// points `window` at globalThis so node's fetch/Response/File stand in for
// the browser's.
//
// Covers the /api run() contract and, specifically, that merge preserves
// submission order for >= 11 uploads (pdf_file_10 must not sort before
// pdf_file_2).
//
// Usage: node wasm/test/wasm-backend-test.js <fixture_a.pdf> <fixture_b.pdf> <outdir>

import { readFile, writeFile } from 'node:fs/promises';
import { pathToFileURL } from 'node:url';
import { strict as assert } from 'node:assert';

const [fixtureA, fixtureB, outDir] = process.argv.slice(2);
assert.ok(fixtureA && fixtureB && outDir, 'usage: wasm-backend-test.js a.pdf b.pdf outdir');

async function writeOut(name, bytes) {
  await writeFile(`${outDir}/${name}`, bytes);
  console.log(`  wrote ${name} (${bytes.length} bytes)`);
}

globalThis.window = globalThis;
await import(pathToFileURL(`${outDir}/wasm-backend.js`));
const backend = window.TSPDF_BACKEND;
assert.ok(backend && backend.supports.includes('merge'), 'backend must install itself');

// A second, direct module instance to build inputs and the expected output.
const { default: createTspdf } = await import(pathToFileURL(`${outDir}/tspdf.js`));
const { makeTspdf } = await import(pathToFileURL(`${outDir}/tspdf-api.js`));
const t = makeTspdf(await createTspdf());

const bytesA = new Uint8Array(await readFile(fixtureA));
const bytesB = new Uint8Array(await readFile(fixtureB));
const ha = t.open(bytesA);
const hb = t.open(bytesB);
const pageA = t.extract(ha, [0]); // one page of layout-text content
const pageB = t.extract(hb, [0]); // one page of image content
t.close(ha);
t.close(hb);

// Twelve single-page uploads, all pageA except slot 2. Under a lexicographic
// key sort, pdf_file_10 and pdf_file_11 land before pdf_file_2, moving the
// odd page from merged position 2 to position 4 — so a byte comparison
// against the correctly-ordered merge catches the reordering.
const N = 12;
const files = {};
for (let i = 0; i < N; i++) {
  const bytes = i === 2 ? pageB : pageA;
  files[`pdf_file_${i}`] = new File([bytes], `f${i}.pdf`, { type: 'application/pdf' });
}

async function backendMerge() {
  const res = await backend.run('merge', {}, files);
  if (!res.ok) assert.fail(`merge failed: ${await res.text()}`);
  return new Uint8Array(await res.arrayBuffer());
}

function expectedMerge() {
  const handles = [];
  try {
    for (let i = 0; i < N; i++) handles.push(t.open(i === 2 ? pageB : pageA));
    return t.merge(handles);
  } finally {
    handles.forEach((h) => t.close(h));
  }
}

// The serializer stamps ModDate with second resolution, so a pair straddling
// a second boundary can legitimately differ; retry so that cannot flake.
let merged = null;
let match = false;
for (let attempt = 0; attempt < 3 && !match; attempt++) {
  const expected = expectedMerge();
  merged = await backendMerge();
  match = merged.length === expected.length &&
    merged.every((byte, i) => byte === expected[i]);
}
assert.ok(match, `merge of ${N} uploads must preserve submission order`);

const hm = t.open(merged);
assert.equal(t.pageCount(hm), N, 'merged page count');
t.close(hm);
await writeOut('out_backend_merged.pdf', merged);

// Unknown tools come back as a non-ok Response, not a throw.
const bad = await backend.run('no-such-tool', {}, {});
assert.equal(bad.ok, false, 'unknown tool must return a non-ok Response');

console.log('wasm-backend-test: all node assertions passed');
