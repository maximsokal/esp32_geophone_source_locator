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

const entries = await readdir(distAssetsDir, { withFileTypes: true });
const jsFiles = entries
  .filter((entry) => entry.isFile() && entry.name.endsWith('.js'))
  .map((entry) => entry.name);
const cssFiles = entries
  .filter((entry) => entry.isFile() && entry.name.endsWith('.css'))
  .map((entry) => entry.name);

if (jsFiles.length !== 1 || cssFiles.length !== 1) {
  throw new Error(
    `Expected one JS and one CSS production asset, got JS=${jsFiles.length} CSS=${cssFiles.length}.`,
  );
}

const jsName = jsFiles[0];
const cssName = cssFiles[0];
const sourceIndex = await readFile(join(distDir, 'index.html'), 'utf8');
const sourceJs = await readFile(join(distAssetsDir, jsName));
const sourceCss = await readFile(join(distAssetsDir, cssName));

const originalJsPath = `./assets/${basename(jsName)}`;
const originalCssPath = `./assets/${basename(cssName)}`;

if (!sourceIndex.includes(originalJsPath) || !sourceIndex.includes(originalCssPath)) {
  throw new Error('Vite index.html does not reference the expected JS/CSS assets.');
}

let espIndex = sourceIndex
  .replaceAll(originalJsPath, './assets/app.js')
  .replaceAll(originalCssPath, './assets/app.css');

const digest = createHash('sha256');
digest.update(espIndex);
digest.update(sourceJs);
digest.update(sourceCss);
const buildId = digest.digest('hex').slice(0, 20);

// Cache-Control: no-store remains the first line of defence on the ESP, but
// Android Chrome can retain a live page/back-forward entry across firmware
// reflashes. Give every firmware WebUI bundle a different asset identity too.
espIndex = espIndex
  .replaceAll('./assets/app.js', `./assets/app.js?v=${buildId}`)
  .replaceAll('./assets/app.css', `./assets/app.css?v=${buildId}`)
  .replace(
    '</head>',
    `<meta name="geolocator-webui-build" content="${buildId}"></head>`,
  );

const gzipOptions = { level: 9, mtime: 0 };
const jsGzip = gzipSync(sourceJs, gzipOptions);
const cssGzip = gzipSync(sourceCss, gzipOptions);

await rm(espDir, { recursive: true, force: true });
await mkdir(espAssetsDir, { recursive: true });

await Promise.all([
  writeFile(join(espDir, 'index.html'), espIndex, 'utf8'),
  writeFile(join(espDir, 'build-id.txt'), `${buildId}\n`, 'utf8'),
  writeFile(join(espAssetsDir, 'app.js.gz'), jsGzip),
  writeFile(join(espAssetsDir, 'app.css.gz'), cssGzip),
]);

console.log(`ESP Web UI bundle prepared: ${buildId}`);
console.log(`  index.html       ${Buffer.byteLength(espIndex)} bytes`);
console.log(`  assets/app.js.gz ${jsGzip.length} bytes`);
console.log(`  assets/app.css.gz ${cssGzip.length} bytes`);
