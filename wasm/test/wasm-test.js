// Node harness for the wasm build: runs every exported operation against two
// native-CLI-generated fixture PDFs and writes the outputs for the qpdf
// conformance gate in run_wasm_test.sh.
//
// Usage: node wasm/test/wasm-test.js <fixture_a.pdf> <fixture_b.pdf> <outdir>

import { readFile, writeFile } from 'node:fs/promises';
import { strict as assert } from 'node:assert';
import createTspdf from '../dist/tspdf.js';
import { makeTspdf } from '../tspdf-api.js';

const [fixtureA, fixtureB, outDir] = process.argv.slice(2);
assert.ok(fixtureA && fixtureB && outDir, 'usage: wasm-test.js a.pdf b.pdf outdir');

const Module = await createTspdf();
const t = makeTspdf(Module);

const PDF_MAGIC = '%PDF-';
function assertPdf(bytes, label) {
  assert.ok(bytes instanceof Uint8Array && bytes.length > 100, `${label}: too small`);
  const head = new TextDecoder().decode(bytes.slice(0, 5));
  assert.equal(head, PDF_MAGIC, `${label}: missing %PDF- header`);
}

async function writeOut(name, bytes) {
  await writeFile(`${outDir}/${name}`, bytes);
  console.log(`  wrote ${name} (${bytes.length} bytes)`);
}

console.log(`tspdf wasm ${t.version()} under node ${process.version}`);

const bytesA = new Uint8Array(await readFile(fixtureA));
const bytesB = new Uint8Array(await readFile(fixtureB));

// open + page_count
const ha = t.open(bytesA);
const hb = t.open(bytesB);
const pagesA = t.pageCount(ha);
const pagesB = t.pageCount(hb);
assert.ok(pagesA >= 1 && pagesB >= 1, 'fixtures must have pages');
console.log(`  fixtures: ${pagesA} + ${pagesB} pages`);

// merge
const merged = t.merge([ha, hb]);
assertPdf(merged, 'merge');
await writeOut('out_merged.pdf', merged);

const hm = t.open(merged);
assert.equal(t.pageCount(hm), pagesA + pagesB, 'merged page count');

// extract (first page of the merged doc)
const extracted = t.extract(hm, [0]);
assertPdf(extracted, 'extract');
const he = t.open(extracted);
assert.equal(t.pageCount(he), 1, 'extracted page count');
t.close(he);
await writeOut('out_extracted.pdf', extracted);

// delete (last page)
const deleted = t.deletePages(hm, [pagesA + pagesB - 1]);
assertPdf(deleted, 'delete');
const hd = t.open(deleted);
assert.equal(t.pageCount(hd), pagesA + pagesB - 1, 'deleted page count');
t.close(hd);
await writeOut('out_deleted.pdf', deleted);

// rotate (all pages, 90 degrees)
const rotated = t.rotate(hm, null, 90);
assertPdf(rotated, 'rotate');
await writeOut('out_rotated.pdf', rotated);

// reorder (swap first two pages)
const order = [...Array(pagesA + pagesB).keys()];
[order[0], order[1]] = [order[1], order[0]];
const reordered = t.reorder(hm, order);
assertPdf(reordered, 'reorder');
await writeOut('out_reordered.pdf', reordered);

// compress
const compressed = t.compress(hm);
assertPdf(compressed, 'compress');
await writeOut('out_compressed.pdf', compressed);

// watermark
const watermarked = t.watermark(hm, 'WASM TEST', { opacity: 0.3 });
assertPdf(watermarked, 'watermark');
await writeOut('out_watermarked.pdf', watermarked);

// metadata: set on a fresh handle, then read back through the JSON view
const hmeta = t.open(merged);
const withMeta = t.setMetadata(hmeta, { title: 'wasm title', author: 'wasm author' });
assertPdf(withMeta, 'set_metadata');
t.close(hmeta);
const hmeta2 = t.open(withMeta);
const meta = t.getMetadata(hmeta2);
assert.equal(meta.title, 'wasm title', 'metadata title roundtrip');
assert.equal(meta.author, 'wasm author', 'metadata author roundtrip');
assert.equal(meta.pages, pagesA + pagesB, 'metadata page count');
t.close(hmeta2);
await writeOut('out_metadata.pdf', withMeta);

// encrypt (AES-128), wrong-password rejection, decrypt roundtrip
const encrypted = t.encrypt(hm, 'wasmpw', 'wasmpw', 128);
assertPdf(encrypted, 'encrypt');
await writeOut('out_encrypted.pdf', encrypted);

assert.throws(() => t.open(encrypted, 'not-the-password'), /wrong password/,
  'wrong password must fail');
assert.throws(() => t.open(encrypted), /password required/,
  'missing password must fail');

const hdec = t.open(encrypted, 'wasmpw');
assert.equal(t.pageCount(hdec), pagesA + pagesB, 'decrypted page count');

// A plain save preserves the source encryption: the result must still
// require the original password.
const resaved = t.save(hdec);
assertPdf(resaved, 'resave encrypted');
assert.throws(() => t.open(resaved), /password required/,
  'plain save of an encrypted doc must stay encrypted');
const hres = t.open(resaved, 'wasmpw');
assert.equal(t.pageCount(hres), pagesA + pagesB, 'resaved page count');
t.close(hres);
await writeOut('out_resaved_encrypted.pdf', resaved);

// saveDecrypted is the explicit opt-out the unlock tool uses.
const decrypted = t.saveDecrypted(hdec);
assertPdf(decrypted, 'decrypt');
t.close(hdec);
const hplain = t.open(decrypted); // no password: must be unencrypted
assert.equal(t.pageCount(hplain), pagesA + pagesB, 'unlocked page count');
t.close(hplain);
await writeOut('out_decrypted.pdf', decrypted);

// error surface: invalid handle and invalid page index give real messages
assert.throws(() => t.pageCount(999), /handle/, 'bad handle error');
assert.throws(() => t.extract(hm, [9999]), Error, 'out-of-range page error');

t.close(hm);
t.close(ha);
t.close(hb);

console.log('wasm-test: all node assertions passed');
