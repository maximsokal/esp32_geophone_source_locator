import { gzipSync } from 'node:zlib';
import { readFile, readdir } from 'node:fs/promises';
import { join, resolve } from 'node:path';

const root = resolve(import.meta.dirname, '..');
const dist = join(root, 'dist');
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

const [scannerKey, scanner] = requireDynamic('src/App.tsx', 'WebXR scanner');
const [objectsKey] = requireDynamic('src/features/objects/ObjectsFeature.tsx', 'Object workspace');
const [sensorsKey] = requireDynamic('src/features/sensors/SensorsFeature.tsx', 'Sensor diagnostics');
const [seismicKey] = requireDynamic('src/features/seismic/LiveSeismicPanel.tsx', 'Live seismic panel');
const [viewportKey] = requireDynamic('src/components/ScanPointCloudViewport.tsx', '3D result viewport');

if ((scanner.css ?? []).length === 0) {
  throw new Error('WebXR scanner must own at least one lazy CSS asset; scan.css leaked into the initial page or was lost.');
}

const [entryKey, entryValue] = appEntry;
for (const dynamicKey of [scannerKey, objectsKey, sensorsKey, seismicKey, viewportKey]) {
  if ((entryValue.imports ?? []).includes(dynamicKey)) {
    throw new Error(`${dynamicKey} unexpectedly appears in the static entry dependency list.`);
  }
}

const assetNames = (await readdir(join(dist, 'assets'))).sort();
const jsNames = assetNames.filter((name) => name.endsWith('.js'));
const cssNames = assetNames.filter((name) => name.endsWith('.css'));
if (jsNames.length < 5) throw new Error(`Expected feature-split JS output, found ${jsNames.length} JS file(s).`);
if (cssNames.length < 2) throw new Error(`Expected initial + lazy CSS output, found ${cssNames.length} CSS file(s).`);

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

const appFile = entryValue.file.replace(/^assets\//, '');
const appRow = rows.find((row) => row.name === appFile);
if (appRow === undefined) throw new Error(`Entry asset ${entryValue.file} not found in dist/assets.`);
if (appRow.rawBytes >= 250_000) {
  throw new Error(`Initial console entry is too large: ${appRow.rawBytes} bytes (target < 250000).`);
}

console.log('Device-console code-split JS assets:');
for (const row of rows) {
  console.log(`  ${row.name.padEnd(48)} raw=${String(row.rawBytes).padStart(8)} gzip=${String(row.gzipBytes).padStart(8)}`);
}
console.log(`Initial console entry: ${appRow.name} raw=${appRow.rawBytes} gzip=${appRow.gzipBytes}`);
console.log(`Total JS: raw=${totalRaw} gzip=${totalGzip}`);
console.log(`CSS assets: ${cssNames.join(', ')}`);
console.log(`Dynamic scanner: ${scannerKey}`);
console.log(`Dynamic objects: ${objectsKey}`);
console.log(`Dynamic sensors: ${sensorsKey}`);
console.log(`Dynamic seismic: ${seismicKey}`);
console.log(`Dynamic 3D viewport: ${viewportKey}`);
console.log(`Entry: ${entryKey}`);
