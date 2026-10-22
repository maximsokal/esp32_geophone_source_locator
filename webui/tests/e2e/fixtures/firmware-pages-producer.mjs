import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../../../..');
const directory = resolve(process.env.FIRMWARE_PAGES_DIR ?? resolve(root, '.test-build/ui-firmware-pages/producer'));
const sha = bytes => createHash('sha256').update(bytes).digest('hex');

export async function loadFirmwarePages() {
  const runPath = resolve(directory, 'run.json');
  const producer = JSON.parse(await readFile(runPath, 'utf8'));
  assert.equal(producer.status, 'PASS', 'Native producer must finish successfully');
  assert.equal(producer.cases.length, 38, 'Incomplete native page inventory');
  for (const [file, digest] of Object.entries(producer.inputs)) {
    assert.equal(sha(await readFile(resolve(root, file))), digest, `Stale native producer input: ${file}`);
  }
  const bodies = {}, assets = {};
  for (const row of producer.cases) {
    const bytes = await readFile(resolve(directory, row.file));
    assert.equal(sha(bytes), row.sha256, `Captured response hash: ${row.id}`);
    bodies[row.id] = bytes.toString('utf8');
  }
  for (const [name, digest] of Object.entries(producer.assets)) {
    const bytes = await readFile(resolve(directory, 'assets', name));
    assert.equal(sha(bytes), digest, `Captured asset hash: ${name}`);
    assets[name] = bytes.toString('utf8');
  }
  return { producer, bodies, assets, runPath };
}
