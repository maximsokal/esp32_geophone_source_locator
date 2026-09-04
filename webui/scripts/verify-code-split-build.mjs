import { gzipSync } from 'node:zlib';
import { readFile, readdir } from 'node:fs/promises';
import { basename, join, resolve } from 'node:path';

const root = resolve(import.meta.dirname, '..');
const dist = join(root, 'dist');
const espDist = join(root, 'esp-dist');
const manifest = JSON.parse(await readFile(join(dist, '.vite', 'manifest.json'), 'utf8'));
const entries = Object.entries(manifest);
const appEntry = entries.find(([, value]) => value.isEntry === true);
if (appEntry === undefined) throw new Error('Vite manifest has no application entry.');

function requireDynamic(sourceSuffix, label) {
  const pair = entries.find(([key]) => key.endsWith(sourceSuffix));
  if (pair === undefined) throw new Error(`${label} is missing from the Vite manifest.`);
  const [key, value] = pair;
  if (value.isDynamicEntry !== true) {
    throw new Error(`${label} must remain a dynamic entry; static import would regress the phone home screen.`);
  }
  return [key, value];
}

function forbidDynamic(sourceSuffix, label) {
  const pair = entries.find(([key]) => key.endsWith(sourceSuffix));
  if (pair !== undefined && pair[1].isDynamicEntry === true) {
    throw new Error(`${label} must be part of the core console bundle; a dynamic chunk can strand the tab on a flaky ESP HTTPS link.`);
  }
}

function embeddedSymbolBase(fileName) {
  const embeddedName = `${fileName}.gz`;
  return `_binary_${embeddedName.replace(/[^A-Za-z0-9_]/g, '_')}`;
}

const [scannerKey, scanner] = requireDynamic('src/App.tsx', 'WebXR scanner');
const [seismicKey] = requireDynamic('src/features/seismic/LiveSeismicPanel.tsx', 'Live seismic panel');
const [viewportKey] = requireDynamic('src/components/ScanPointCloudViewport.tsx', '3D result viewport');
forbidDynamic('src/features/objects/ObjectsFeature.tsx', 'Object workspace');
forbidDynamic('src/features/sensors/SensorsFeature.tsx', 'Sensor diagnostics');

if ((scanner.css ?? []).length === 0) {
  throw new Error('WebXR scanner must own at least one lazy CSS asset; scan.css leaked into the initial page or was lost.');
}

const [entryKey, entryValue] = appEntry;
for (const dynamicKey of [scannerKey, seismicKey, viewportKey]) {
  if ((entryValue.imports ?? []).includes(dynamicKey)) {
    throw new Error(`${dynamicKey} unexpectedly appears in the static entry dependency list.`);
  }
}

const indexHtml = await readFile(join(dist, 'index.html'), 'utf8');
if (/modulepreload/i.test(indexHtml)) {
  throw new Error(
    'Initial HTML contains modulepreload links; they can create a speculative parallel TLS burst against the ESP32 HTTPS server.',
  );
}

const assetNames = (await readdir(join(dist, 'assets'))).sort();
const jsNames = assetNames.filter((name) => name.endsWith('.js'));
const cssNames = assetNames.filter((name) => name.endsWith('.css'));
if (jsNames.length < 5) throw new Error(`Expected feature-split JS output, found ${jsNames.length} JS file(s).`);
if (cssNames.length < 2) throw new Error(`Expected initial + lazy CSS output, found ${cssNames.length} CSS file(s).`);

const initialVendorNames = jsNames.filter((name) => name.startsWith('initial-vendor-'));
if (initialVendorNames.length !== 1) {
  throw new Error(
    `Expected exactly one initial-vendor JS request for the constrained ESP HTTPS path, found ${initialVendorNames.length}: ${initialVendorNames.join(', ') || '(none)'}.`,
  );
}

let totalRaw = 0;
let totalGzip = 0;
const rows = [];
for (const name of jsNames) {
  const data = await readFile(join(dist, 'assets', name));
  const gzipBytes = gzipSync(data, { level: 9, mtime: 0 }).length;
  totalRaw += data.length;
  totalGzip += gzipBytes;
  rows.push({ name, rawBytes: data.length, gzipBytes });
}

const appFile = basename(entryValue.file);
const appRow = rows.find((row) => row.name === appFile);
if (appRow === undefined) throw new Error(`Entry asset ${entryValue.file} not found in dist/assets.`);
if (appRow.rawBytes >= 250_000) {
  throw new Error(`Initial console entry is too large: ${appRow.rawBytes} bytes (target < 250000).`);
}

const initialCssNames = (entryValue.css ?? []).map((path) => basename(path));
if (initialCssNames.length !== 1 || !cssNames.includes(initialCssNames[0])) {
  throw new Error(`Expected exactly one initial CSS asset, got ${initialCssNames.join(', ') || '(none)'}.`);
}
const secondaryNames = [
  ...jsNames.filter((name) => name !== appFile),
  ...cssNames.filter((name) => !initialCssNames.includes(name)),
].sort();
if (secondaryNames.length === 0) {
  throw new Error('Expected secondary lazy/code-split assets for ESP embedding.');
}

const espAssetNames = (await readdir(join(espDist, 'assets'))).sort();
const generatedChunkTable = await readFile(
  join(espDist, 'generated_webui_chunk_table.inc'),
  'utf8',
);
if (generatedChunkTable.includes('_binary_generated_webui_chunks_')) {
  throw new Error(
    'ESP chunk table still contains path-prefixed _binary_generated_webui_chunks_* symbols; ESP-IDF 5.5.4 emits basename-only symbols.',
  );
}

for (const name of secondaryNames) {
  const gzipName = `${name}.gz`;
  if (!espAssetNames.includes(gzipName)) {
    throw new Error(`Prepared ESP asset is missing: esp-dist/assets/${gzipName}.`);
  }
  const gzipData = await readFile(join(espDist, 'assets', gzipName));
  if (gzipData.length === 0) {
    throw new Error(`Prepared ESP asset is empty: esp-dist/assets/${gzipName}.`);
  }

  const symbol = embeddedSymbolBase(name);
  const startDeclaration = `asm("${symbol}_start")`;
  const endDeclaration = `asm("${symbol}_end")`;
  if (!generatedChunkTable.includes(startDeclaration)) {
    throw new Error(`Missing ESP-IDF basename start symbol for ${gzipName}: ${symbol}_start.`);
  }
  if (!generatedChunkTable.includes(endDeclaration)) {
    throw new Error(`Missing ESP-IDF basename end symbol for ${gzipName}: ${symbol}_end.`);
  }
}

console.log('Device-console code-split JS assets:');
for (const row of rows) {
  console.log(`  ${row.name.padEnd(48)} raw=${String(row.rawBytes).padStart(8)} gzip=${String(row.gzipBytes).padStart(8)}`);
}
console.log(`Initial console entry: ${appRow.name} raw=${appRow.rawBytes} gzip=${appRow.gzipBytes}`);
console.log(`Initial vendor requests: ${initialVendorNames.length} (${initialVendorNames.join(', ')})`);
console.log('Initial HTML modulepreload: disabled');
console.log(`Total JS: raw=${totalRaw} gzip=${totalGzip}`);
console.log(`CSS assets: ${cssNames.join(', ')}`);
console.log(`ESP secondary assets: ${secondaryNames.length} basename-symbol pairs verified`);
console.log('Core console tabs: Objects + Sensors are statically reachable from app.js');
console.log(`Dynamic scanner: ${scannerKey}`);
console.log(`Dynamic seismic: ${seismicKey}`);
console.log(`Dynamic 3D viewport: ${viewportKey}`);
console.log(`Entry: ${entryKey}`);
