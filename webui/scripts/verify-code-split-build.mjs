import { gzipSync } from 'node:zlib';
import { readFile, readdir } from 'node:fs/promises';
import { join, resolve } from 'node:path';

const root = resolve(import.meta.dirname, '..');
const dist = join(root, 'dist');
const manifest = JSON.parse(await readFile(join(dist, '.vite', 'manifest.json'), 'utf8'));
const entries = Object.entries(manifest);
const appEntry = entries.find(([, value]) => value.isEntry === true);
if (appEntry === undefined) throw new Error('Vite manifest has no application entry.');

const viewportKey = entries.find(([key]) => key.endsWith('src/components/ScanPointCloudViewport.tsx'))?.[0];
if (viewportKey === undefined) throw new Error('ScanPointCloudViewport is missing from the Vite manifest.');
const viewport = manifest[viewportKey];
if (viewport.isDynamicEntry !== true) {
  throw new Error('ScanPointCloudViewport must remain a dynamic entry; static import would regress initial load.');
}

const [entryKey, entryValue] = appEntry;
if ((entryValue.dynamicImports ?? []).includes(viewportKey) === false) {
  throw new Error(`Application entry ${entryKey} does not dynamically import the 3D viewport.`);
}
if ((entryValue.imports ?? []).includes(viewportKey)) {
  throw new Error('3D viewport unexpectedly appears in the static entry dependency list.');
}

const assetNames = (await readdir(join(dist, 'assets'))).filter((name) => name.endsWith('.js')).sort();
if (assetNames.length < 2) throw new Error(`Expected split JS output, found ${assetNames.length} JS file(s).`);

let totalRaw = 0;
let totalGzip = 0;
const rows = [];
for (const name of assetNames) {
  const data = await readFile(join(dist, 'assets', name));
  const gzipBytes = gzipSync(data, { level: 9, mtime: 0 }).length;
  totalRaw += data.length;
  totalGzip += gzipBytes;
  rows.push({ name, rawBytes: data.length, gzipBytes });
}

const appFile = entryValue.file.replace(/^assets\//, '');
const appRow = rows.find((row) => row.name === appFile);
if (appRow === undefined) throw new Error(`Entry asset ${entryValue.file} not found in dist/assets.`);
if (appRow.rawBytes >= 500_000) {
  throw new Error(`Initial app entry is still too large: ${appRow.rawBytes} bytes (target < 500000).`);
}

console.log('Code-split JS assets:');
for (const row of rows) {
  console.log(`  ${row.name.padEnd(48)} raw=${String(row.rawBytes).padStart(8)} gzip=${String(row.gzipBytes).padStart(8)}`);
}
console.log(`Initial entry: ${appRow.name} raw=${appRow.rawBytes} gzip=${appRow.gzipBytes}`);
console.log(`Total JS: raw=${totalRaw} gzip=${totalGzip}`);
console.log(`Lazy viewport manifest key: ${viewportKey}`);
