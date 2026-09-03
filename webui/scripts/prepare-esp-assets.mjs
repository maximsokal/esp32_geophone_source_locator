import { createHash } from 'node:crypto';
import { mkdir, readdir, readFile, rm, writeFile } from 'node:fs/promises';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { gzipSync } from 'node:zlib';

const scriptDir = dirname(fileURLToPath(import.meta.url));
const webuiDir = resolve(scriptDir, '..');
const distDir = join(webuiDir, 'dist');
const distAssetsDir = join(distDir, 'assets');
const espDir = join(webuiDir, 'esp-dist');
const espAssetsDir = join(espDir, 'assets');
const MAX_SECONDARY_ASSETS = 32;

const manifest = JSON.parse(await readFile(join(distDir, '.vite', 'manifest.json'), 'utf8'));
const appEntry = Object.values(manifest).find((value) => value.isEntry === true);
if (appEntry === undefined) throw new Error('Vite manifest has no application entry.');

const entries = await readdir(distAssetsDir, { withFileTypes: true });
const jsFiles = entries
  .filter((entry) => entry.isFile() && entry.name.endsWith('.js'))
  .map((entry) => entry.name)
  .sort();
const cssFiles = entries
  .filter((entry) => entry.isFile() && entry.name.endsWith('.css'))
  .map((entry) => entry.name)
  .sort();

if (!jsFiles.includes('app.js')) {
  throw new Error(`Expected stable Vite entry assets/app.js, got JS=${jsFiles.join(', ') || '(none)'}.`);
}

const initialCss = (appEntry.css ?? []).map((path) => basename(path));
if (initialCss.length !== 1 || !cssFiles.includes(initialCss[0])) {
  throw new Error(`Expected exactly one initial CSS asset from the Vite manifest, got ${initialCss.join(', ') || '(none)'}.`);
}
const cssName = initialCss[0];

const secondaryNames = [
  ...jsFiles.filter((name) => name !== 'app.js'),
  ...cssFiles.filter((name) => name !== cssName),
].sort();
if (secondaryNames.length === 0) {
  throw new Error('Expected secondary lazy/code-split assets; build regressed to a single entry payload.');
}
if (secondaryNames.length > MAX_SECONDARY_ASSETS) {
  throw new Error(`Refusing to embed ${secondaryNames.length} secondary assets; bounded limit is ${MAX_SECONDARY_ASSETS}.`);
}

function contentType(name) {
  if (name.endsWith('.js')) return 'application/javascript; charset=utf-8';
  if (name.endsWith('.css')) return 'text/css; charset=utf-8';
  throw new Error(`Unsupported secondary asset type: ${name}`);
}

const sourceIndex = await readFile(join(distDir, 'index.html'), 'utf8');
const sourceJs = await readFile(join(distAssetsDir, 'app.js'));
const sourceCss = await readFile(join(distAssetsDir, cssName));
const secondaryAssets = await Promise.all(
  secondaryNames.map(async (name) => ({
    name,
    type: contentType(name),
    data: await readFile(join(distAssetsDir, name)),
  })),
);

const originalJsPath = './assets/app.js';
const originalCssPath = `./assets/${cssName}`;
if (!sourceIndex.includes(originalJsPath) || !sourceIndex.includes(originalCssPath)) {
  throw new Error('Vite index.html does not reference the expected entry JS/CSS assets.');
}

let espIndex = sourceIndex.replaceAll(originalCssPath, './assets/app.css');

const digest = createHash('sha256');
const digestSeparator = Uint8Array.of(0);
digest.update(espIndex);
digest.update('app.js');
digest.update(digestSeparator);
digest.update(sourceJs);
digest.update('app.css');
digest.update(digestSeparator);
digest.update(sourceCss);
for (const asset of secondaryAssets) {
  digest.update(asset.name);
  digest.update(digestSeparator);
  digest.update(asset.data);
}
const buildId = digest.digest('hex').slice(0, 20);

espIndex = espIndex
  .replaceAll('./assets/app.js', `./assets/app.js?v=${buildId}`)
  .replaceAll('./assets/app.css', `./assets/app.css?v=${buildId}`)
  .replace('</head>', `<meta name="geolocator-webui-build" content="${buildId}"></head>`);

const gzipOptions = { level: 9, mtime: 0 };
const jsGzip = gzipSync(sourceJs, gzipOptions);
const cssGzip = gzipSync(sourceCss, gzipOptions);
const secondaryGzip = secondaryAssets.map((asset) => ({
  ...asset,
  data: gzipSync(asset.data, gzipOptions),
}));

function cString(value) {
  return JSON.stringify(value);
}
function embeddedSymbolBase(fileName) {
  // ESP-IDF 5.5.4 target_add_binary_data() turns an EMBED_FILES path into an
  // absolute path and then uses only its basename when generating _binary_*
  // symbols. Match that emitted basename exactly; directories are not part of it.
  const embeddedName = `${fileName}.gz`;
  return `_binary_${embeddedName.replace(/[^A-Za-z0-9_]/g, '_')}`;
}

const declarations = [];
const tableRows = [];
secondaryGzip.forEach((asset, index) => {
  const alias = `webui_chunk_${index}`;
  const symbol = embeddedSymbolBase(asset.name);
  declarations.push(`extern const unsigned char ${alias}_start[] asm(${cString(`${symbol}_start`)});`);
  declarations.push(`extern const unsigned char ${alias}_end[] asm(${cString(`${symbol}_end`)});`);
  tableRows.push(
    `    { ${cString(`/assets/${asset.name}`)}, ${cString(`/assets/${asset.name}.gz`)}, ` +
    `${cString(asset.type)}, ${alias}_start, ${alias}_end },`,
  );
});
const generatedTable = [
  ...declarations,
  '',
  'static const web_webui_chunk_asset_t s_webui_chunk_assets[] = {',
  ...tableRows,
  '};',
  '#define WEB_WEBUI_CHUNK_ASSET_COUNT (sizeof(s_webui_chunk_assets) / sizeof(s_webui_chunk_assets[0]))',
  '',
].join(String.fromCharCode(10));

await rm(espDir, { recursive: true, force: true });
await mkdir(espAssetsDir, { recursive: true });

await Promise.all([
  writeFile(join(espDir, 'index.html'), espIndex, 'utf8'),
  writeFile(join(espDir, 'build-id.txt'), `${buildId}${String.fromCharCode(10)}`, 'utf8'),
  writeFile(join(espDir, 'generated_webui_chunk_table.inc'), generatedTable, 'utf8'),
  writeFile(join(espAssetsDir, 'app.js.gz'), jsGzip),
  writeFile(join(espAssetsDir, 'app.css.gz'), cssGzip),
  ...secondaryGzip.map((asset) => writeFile(join(espAssetsDir, `${asset.name}.gz`), asset.data)),
]);

console.log(`ESP Web UI bundle prepared: ${buildId}`);
console.log(`  index.html        ${Buffer.byteLength(espIndex)} bytes`);
console.log(`  assets/app.js.gz  ${jsGzip.length} bytes`);
console.log(`  assets/app.css.gz ${cssGzip.length} bytes`);
let secondaryTotal = 0;
for (const asset of secondaryGzip) {
  secondaryTotal += asset.data.length;
  console.log(`  assets/${asset.name}.gz ${asset.data.length} bytes (${asset.type})`);
}
console.log(`  secondary assets  ${secondaryGzip.length} files / ${secondaryTotal} gzip bytes`);
