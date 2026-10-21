import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdir, mkdtemp, readFile, writeFile, rm, realpath } from 'node:fs/promises';
import { resolve, join, sep } from 'node:path';
import { createHash } from 'node:crypto';
import { sourceInputs, checkEnvironment, baselineErrors, verifiedCandidate, stability, approveCandidate } from '../scripts/run-ui-visual.mjs';

const sha = (bytes) => createHash('sha256').update(bytes).digest('hex');
const png = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=', 'base64');
async function fixture(t) {
  const base = resolve('../.test-build/ui-visual-unit'); await mkdir(base, { recursive: true });
  const root = await mkdtemp(join(base, 'case-'));
  t.after(async () => { const target = await realpath(root); assert.ok(target.startsWith(await realpath(base) + sep)); await rm(target, { recursive: true, force: true }); });
  await mkdir(join(root, 'src')); await writeFile(join(root, 'src/main.ts'), 'export const value=1;');
  const snapshots = join(root, 'tests/e2e/snapshots'); await mkdir(snapshots, { recursive: true });
  const canonical = { id: 'isolated-test-environment' };
  const manifest = { schema: 1, environment: canonical, approval: null };
  await writeFile(join(snapshots, 'manifest.json'), JSON.stringify(manifest));
  return { root, snapshots, manifest, canonical };
}
async function candidate(root, name, canonical, bytes = png) {
  const directory = join(root, name); await mkdir(directory);
  await writeFile(join(directory, 'actual.png'), bytes);
  const body = { schema: 1, status: 'CAPTURED_NOT_APPROVED', canonical, environment: { test: true }, inputs: await sourceInputs(root),
    cases: [{ id: 'en-home-1x1', file: 'en-home-1x1.png', image: 'actual.png', sha256: sha(bytes), state: { width: 1, height: 1 } }] };
  const record = { ...body, candidateId: sha(JSON.stringify(body)) };
  await writeFile(join(directory, 'candidate.json'), JSON.stringify(record));
  return { directory, record };
}

test('canonical environment rejects a different OS, Node, browser package or fonts', () => {
  const expected = { platform: 'linux', arch: 'x64', node: 'v24', os: 'ubuntu', osVersion: '24.04', fontsSha256: 'fonts', playwright: '1.64.1', image: 'pinned' };
  assert.doesNotThrow(() => checkEnvironment(expected, expected));
  for (const key of Object.keys(expected)) assert.throws(() => checkEnvironment(expected, { ...expected, [key]: 'wrong' }), /environment mismatch/);
});
test('missing/unapproved references are errors without writing any PNG', async (t) => {
  const { root, manifest } = await fixture(t);
  assert.match((await baselineErrors(root, manifest))['*'], /No explicitly approved/);
  manifest.approval = { files: { 'en-home-1x1.png': { sha256: sha(png) } } };
  assert.match((await baselineErrors(root, manifest))['en-home-1x1.png'], /missing/);
});
test('text inputs tolerate checkout line endings but detect real source changes', async (t) => {
  const { root } = await fixture(t);
  await writeFile(join(root, 'src/main.ts'), 'export const value=1;\n');
  const before = await sourceInputs(root);
  await writeFile(join(root, 'src/main.ts'), 'export const value=1;\r\n');
  assert.deepEqual(await sourceInputs(root), before);
  await writeFile(join(root, 'src/main.ts'), 'export const value=2;\r\n');
  assert.notDeepEqual(await sourceInputs(root), before);
  const changed = await sourceInputs(root);
  await writeFile(join(root, 'index.html'), '<meta name="viewport" content="width=device-width">');
  assert.notDeepEqual(await sourceInputs(root), changed);
  const withHtml = await sourceInputs(root);
  await mkdir(join(root, 'public'));
  await writeFile(join(root, 'public/icon.png'), png);
  assert.notDeepEqual(await sourceInputs(root), withHtml);
});
test('changed PNG bytes and escaping paths cannot be approved', async (t) => {
  const { root, canonical } = await fixture(t);
  const { directory } = await candidate(root, 'first', canonical);
  await writeFile(join(directory, 'actual.png'), 'changed');
  await assert.rejects(verifiedCandidate(directory), /PNG changed/);
  const escaped = await candidate(root, 'escaped', canonical);
  const body = { ...escaped.record, cases: [{ ...escaped.record.cases[0], image: '../first/actual.png' }] };
  delete body.candidateId;
  await writeFile(join(escaped.directory, 'candidate.json'), JSON.stringify({ ...body, candidateId: sha(JSON.stringify(body)) }));
  await assert.rejects(verifiedCandidate(escaped.directory), /escapes/);
});
test('repeat capture must match every PNG; unstable captures cannot pass', async (t) => {
  const { root, canonical } = await fixture(t);
  const a = await candidate(root, 'a', canonical);
  const b = await candidate(root, 'b', canonical);
  assert.equal((await stability(a.directory, b.directory)).status, 'STABLE');
  const changed = Buffer.from(png); changed[changed.length - 1] ^= 1;
  const c = await candidate(root, 'c', canonical, changed);
  await assert.rejects(stability(a.directory, c.directory), /unstable/);
});
test('approval requires reviewed ID, stable repeat and unchanged source before any writes', async (t) => {
  const { root, canonical, snapshots } = await fixture(t);
  const a = await candidate(root, 'a', canonical), b = await candidate(root, 'b', canonical);
  await stability(a.directory, b.directory);
  const original = await readFile(join(snapshots, 'manifest.json'));
  await assert.rejects(approveCandidate(root, a.directory, 'not-the-reviewed-id', 'all'), /reviewed candidate ID/);
  await writeFile(join(root, 'src/main.ts'), 'changed source');
  await assert.rejects(approveCandidate(root, a.directory, a.record.candidateId, 'all'), /Source changed/);
  assert.deepEqual(await readFile(join(snapshots, 'manifest.json')), original);
});
test('explicit approval copies only selected reviewed PNGs in an isolated fixture', async (t) => {
  const { root, canonical, snapshots } = await fixture(t);
  const a = await candidate(root, 'a', canonical), b = await candidate(root, 'b', canonical);
  await stability(a.directory, b.directory);
  await assert.rejects(approveCandidate(root, a.directory, a.record.candidateId, ['not-reviewed.png']), /exact reviewed filenames/);
  assert.equal(await approveCandidate(root, a.directory, a.record.candidateId, ['en-home-1x1.png']), 1);
  assert.deepEqual(await readFile(join(snapshots, canonical.id, 'en-home-1x1.png')), png);
  const manifest = JSON.parse(await readFile(join(snapshots, 'manifest.json'), 'utf8'));
  assert.deepEqual(await baselineErrors(root, manifest), {});
  await writeFile(join(snapshots, canonical.id, 'en-home-1x1.png'), 'unreviewed replacement');
  assert.match((await baselineErrors(root, manifest))['en-home-1x1.png'], /hash changed/);
});
test('invalid environment and an existing approval lock prevent PNG writes', async (t) => {
  const { root, canonical, snapshots, manifest } = await fixture(t);
  const a = await candidate(root, 'a', canonical), b = await candidate(root, 'b', canonical);
  await stability(a.directory, b.directory);
  await writeFile(join(snapshots, 'manifest.json.pending'), 'prior incomplete approval');
  await assert.rejects(approveCandidate(root, a.directory, a.record.candidateId, 'all'), /EEXIST/);
  await assert.rejects(readFile(join(snapshots, canonical.id, 'en-home-1x1.png')), /ENOENT/);
  const foreign = { ...manifest, environment: { id: '../outside' } };
  await writeFile(join(snapshots, 'manifest.json'), JSON.stringify(foreign));
  const c = await candidate(root, 'c', foreign.environment), d = await candidate(root, 'd', foreign.environment);
  await stability(c.directory, d.directory);
  await assert.rejects(approveCandidate(root, c.directory, c.record.candidateId, 'all'), /Invalid reference environment/);
});
