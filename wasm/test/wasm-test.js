// Node harness for the wasm build: runs every exported operation against two
// native-CLI-generated fixture PDFs and writes the outputs for the qpdf
// conformance gate in run_wasm_test.sh.
//
// Usage: node wasm/test/wasm-test.js <fixture_a.pdf> <fixture_b.pdf> <rotate90.pdf> <rotate90_wm_ref.pdf> <xmp_meta_full.pdf> <outdir>

import { readFile, writeFile } from 'node:fs/promises';
import { strict as assert } from 'node:assert';
import createTspdf from '../dist/tspdf.js';
import { makeTspdf } from '../tspdf-api.js';

const [fixtureA, fixtureB, fixtureRotate90, fixtureRotate90WmRef, fixtureXmp, outDir] = process.argv.slice(2);
assert.ok(fixtureA && fixtureB && fixtureRotate90 && fixtureRotate90WmRef && fixtureXmp && outDir,
  'usage: wasm-test.js a.pdf b.pdf rotate90.pdf rotate90_wm_ref.pdf xmp_meta_full.pdf outdir');

// Extract the first `cm` matrix [a,b,c,d,e,f] from a PDF's content streams.
// Content streams may be compressed; we search raw bytes for uncompressed
// content blocks prepended by the shim (which are NOT deflate-compressed).
// Returns [a, b, c, d, e, f] or null.
function extractFirstCmMatrix(pdfBytes) {
  // The watermark content stream is appended as a new uncompressed stream.
  // Find 'cm' in the raw bytes (ASCII text). We scan for the ASCII sequence
  // matching: <number> <number> <number> <number> <number> <number> cm
  // Latin-1 decoding is intentional: it never throws on null bytes, which
  // are the compression sentinel in deflate streams, so the raw byte scan
  // works across both uncompressed and compressed content without early exit.
  const text = new TextDecoder('latin1').decode(pdfBytes);
  // Match 6 floating-point/integer numbers before 'cm' with whitespace
  const re = /([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+cm/g;
  let m;
  while ((m = re.exec(text)) !== null) {
    return [m[1], m[2], m[3], m[4], m[5], m[6]].map(Number);
  }
  return null;
}

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
const bytesRotate90 = new Uint8Array(await readFile(fixtureRotate90));
const bytesRotate90WmRef = new Uint8Array(await readFile(fixtureRotate90WmRef));
const bytesXmp = new Uint8Array(await readFile(fixtureXmp));

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

// XMP sync: setting metadata on a doc that carries an XMP packet must update
// the packet too, not just the Info dictionary. The xmp_meta_full fixture has
// dc:title "Old XMP title" in its XMP stream; after setMetadata the saved
// bytes must carry the new value there. This was the live wasm bug: the shim
// called reader setters directly without invoking sync, leaving XMP stale.
{
  const hxmp = t.open(bytesXmp);
  const withXmp = t.setMetadata(hxmp, { title: 'New wasm title' });
  t.close(hxmp);
  assertPdf(withXmp, 'set_metadata_xmp_sync');
  await writeOut('out_metadata_xmp.pdf', withXmp);

  // The XMP packet uses the element form:
  //   <rdf:li xml:lang="x-default">VALUE</rdf:li>
  // after sync the value must be the new title; decode as Latin-1 since the
  // null-assert in the Latin-1 scan is the compression sentinel (not reached
  // here because the packet is uncompressed plain text).
  const xmpText = new TextDecoder('latin1').decode(withXmp);
  assert.ok(
    xmpText.includes('x-default">New wasm title</rdf:li>'),
    'set_metadata_xmp_sync: XMP dc:title not updated — XMP packet still stale'
  );
  console.log('  set_metadata_xmp_sync: XMP dc:title updated in packet');
}

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

// /Rotate 90 watermark: TDD gate for the "wasm watermark ignores page /Rotate"
// bug. The CLI reference (bytesRotate90WmRef) was produced by
//   tspdf rotate fixture_a --angle 90 | tspdf watermark --text "ROTATE TEST"
// which uses tsops_watermark_text and therefore compensates /Rotate 90 with
// angle = 45 + 90 = 135 degrees (cos(135) ≈ -0.7071).
//
// The buggy shim always used 45 degrees (cos(45) ≈ +0.7071). After the fix the
// shim delegates to tsops_watermark_text and produces the same matrix.
//
// Assertion: the first `cm` matrix in the wasm-watermarked /Rotate 90 output
// has a negative first element (cos(135°) < 0). The CLI reference must also
// have a negative first element (confirms the fixture is good). If the shim
// still uses 45° the first element will be positive and the assertion fails.
{
  const hr90 = t.open(bytesRotate90);
  const wasm_wm90 = t.watermark(hr90, 'ROTATE TEST', { opacity: 0.3 });
  t.close(hr90);
  assertPdf(wasm_wm90, 'watermark_rotate90');
  await writeOut('out_watermarked_rotate90.pdf', wasm_wm90);

  // Extract the cm matrix from the wasm output and from the CLI reference.
  const wasmMatrix = extractFirstCmMatrix(wasm_wm90);
  const refMatrix  = extractFirstCmMatrix(bytesRotate90WmRef);
  assert.ok(wasmMatrix !== null, 'watermark_rotate90: no cm matrix found in wasm output');
  assert.ok(refMatrix  !== null, 'watermark_rotate90: no cm matrix found in CLI reference');

  // The CLI reference must use a negative cosine (135°), confirming the fixture.
  assert.ok(refMatrix[0] < 0,
    `watermark_rotate90: CLI reference cm[0]=${refMatrix[0]} should be negative (135 deg)`);

  // The wasm shim must now also use a negative cosine (bug fix: honors /Rotate).
  assert.ok(wasmMatrix[0] < 0,
    `watermark_rotate90: wasm cm[0]=${wasmMatrix[0]} should be negative (135 deg); ` +
    'shim is not compensating /Rotate — bug not fixed');

  // The matrices must match within floating-point tolerance (all 6 elements:
  // a,b,c,d are the rotation coefficients; e,f are the page-center translation).
  for (let i = 0; i < 6; i++) {
    assert.ok(Math.abs(wasmMatrix[i] - refMatrix[i]) < 1e-4,
      `watermark_rotate90: cm[${i}] wasm=${wasmMatrix[i]} ref=${refMatrix[i]} differ`);
  }
  console.log(`  watermark_rotate90: cm=[${wasmMatrix.map(v=>v.toFixed(4)).join(',')}] matches CLI`);
}

// error surface: invalid handle and invalid page index give real messages
assert.throws(() => t.pageCount(999), /handle/, 'bad handle error');
assert.throws(() => t.extract(hm, [9999]), Error, 'out-of-range page error');

t.close(hm);
t.close(ha);
t.close(hb);

console.log('wasm-test: all node assertions passed');
