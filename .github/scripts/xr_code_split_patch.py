from pathlib import Path

ROOT = Path.cwd()


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def write(path: str, content: str) -> None:
    (ROOT / path).write_text(content, encoding="utf-8")


def replace_once(path: str, old: str, new: str) -> None:
    content = read(path)
    count = content.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected exactly one replacement target, found {count}: {old[:120]!r}")
    write(path, content.replace(old, new, 1))


# 1) Defer the Three.js result renderer until a measured scan actually exists.
replace_once(
    "webui/src/App.tsx",
    "import { useCallback, useEffect, useState } from 'react';",
    "import { Component, lazy, Suspense, useCallback, useEffect, useState, type ErrorInfo, type ReactNode } from 'react';",
)
replace_once(
    "webui/src/App.tsx",
    "import { ScanPointCloudViewport } from './components/ScanPointCloudViewport';\n",
    "",
)
replace_once(
    "webui/src/App.tsx",
    "import type { ScanPointCloudSnapshot, WebXrCapabilities } from './xr/webxrRuntime';\n\nfunction formatMeters",
    """import type { ScanPointCloudSnapshot, WebXrCapabilities } from './xr/webxrRuntime';

const ScanPointCloudViewport = lazy(async () => {
  const module = await import('./components/ScanPointCloudViewport');
  return { default: module.ScanPointCloudViewport };
});

class ResultViewportErrorBoundary extends Component<
  { children: ReactNode },
  { message: string | null }
> {
  state: { message: string | null } = { message: null };

  static getDerivedStateFromError(error: unknown): { message: string } {
    return { message: error instanceof Error ? error.message : String(error) };
  }

  componentDidCatch(error: unknown, info: ErrorInfo): void {
    clientDiagnostics.warn('SCAN_RESULT_RENDERER_LOAD_FAILED', {
      error: error instanceof Error ? error.message : String(error),
      componentStack: info.componentStack ?? '',
    });
    void clientDiagnostics.flush();
  }

  render(): ReactNode {
    if (this.state.message !== null) {
      return (
        <div className=\"viewport-shell scan-viewport\" role=\"alert\">
          3D result renderer could not be loaded. Reload the page to retry. ({this.state.message})
        </div>
      );
    }
    return this.props.children;
  }
}

function formatMeters""",
)
replace_once(
    "webui/src/App.tsx",
    """          <ScanPointCloudViewport
            snapshot={scanSnapshot}
            viewCommand={viewCommand}
            display={display}
            mode={scanMode}
          />""",
    """          <ResultViewportErrorBoundary key={scanSnapshot.pointCloudVersion}>
            <Suspense
              fallback={
                <div className=\"viewport-shell scan-viewport\" role=\"status\" aria-live=\"polite\">
                  Loading 3D result renderer…
                </div>
              }
            >
              <ScanPointCloudViewport
                snapshot={scanSnapshot}
                viewCommand={viewCommand}
                display={display}
                mode={scanMode}
              />
            </Suspense>
          </ResultViewportErrorBoundary>""",
)

# 2) Vite 8 / Rolldown chunking: keep initial dependencies separate from lazy
# dependencies and cap multi-module vendor chunks. Dynamic imports remain the
# semantic boundary; groups only make the resulting network payloads bounded.
write(
    "webui/vite.config.ts",
    """import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  base: './',
  plugins: [react()],
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    sourcemap: false,
    manifest: true,
    target: 'es2022',
    assetsDir: 'assets',
    rolldownOptions: {
      output: {
        entryFileNames: 'assets/app.js',
        chunkFileNames: 'assets/[name]-[hash].js',
        codeSplitting: {
          groups: [
            {
              name: 'initial-vendor',
              test: /node_modules[\\/]/,
              tags: ['$initial'],
              maxSize: 320_000,
              priority: 20,
            },
            {
              name: 'lazy-vendor',
              test: /node_modules[\\/]/,
              maxSize: 420_000,
              priority: 10,
            },
          ],
        },
      },
    },
  },
});
""",
)

# 3) Production bundle regression: verify the 3D renderer is a dynamic entry,
# the initial entry no longer contains it, and all browser JS is chunked.
write(
    "webui/scripts/verify-code-split-build.mjs",
    """import { gzipSync } from 'node:zlib';
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
""",
)
replace_once(
    "webui/package.json",
    '    "typecheck": "node ./node_modules/typescript/bin/tsc --noEmit",\n',
    '    "typecheck": "node ./node_modules/typescript/bin/tsc --noEmit",\n    "verify:bundle": "node scripts/verify-code-split-build.mjs",\n',
)

# 4) Package every additional JS chunk for ESP flash. app.js/app.css retain their
# stable aliases for compatibility; dynamic/static secondary chunks keep Vite's
# content-hashed names so stale browser module graphs cannot reuse old code.
write(
    "webui/scripts/prepare-esp-assets.mjs",
    """import { createHash } from 'node:crypto';
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
const MAX_SECONDARY_JS_CHUNKS = 24;

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
if (cssFiles.length !== 1) {
  throw new Error(`Expected one production CSS asset, got CSS=${cssFiles.length}.`);
}
const secondaryJsFiles = jsFiles.filter((name) => name !== 'app.js');
if (secondaryJsFiles.length === 0) {
  throw new Error('Expected at least one secondary JS chunk; lazy/code-split build regressed to a single bundle.');
}
if (secondaryJsFiles.length > MAX_SECONDARY_JS_CHUNKS) {
  throw new Error(`Refusing to embed ${secondaryJsFiles.length} secondary JS chunks; bounded limit is ${MAX_SECONDARY_JS_CHUNKS}.`);
}

const cssName = cssFiles[0];
const sourceIndex = await readFile(join(distDir, 'index.html'), 'utf8');
const sourceJs = await readFile(join(distAssetsDir, 'app.js'));
const sourceCss = await readFile(join(distAssetsDir, cssName));
const secondaryAssets = await Promise.all(
  secondaryJsFiles.map(async (name) => ({ name, data: await readFile(join(distAssetsDir, name)) })),
);

const originalJsPath = './assets/app.js';
const originalCssPath = `./assets/${basename(cssName)}`;
if (!sourceIndex.includes(originalJsPath) || !sourceIndex.includes(originalCssPath)) {
  throw new Error('Vite index.html does not reference the expected entry JS/CSS assets.');
}

let espIndex = sourceIndex
  .replaceAll(originalCssPath, './assets/app.css');

const digest = createHash('sha256');
digest.update(espIndex);
digest.update('app.js\0');
digest.update(sourceJs);
digest.update('app.css\0');
digest.update(sourceCss);
for (const asset of secondaryAssets) {
  digest.update(`${asset.name}\0`);
  digest.update(asset.data);
}
const buildId = digest.digest('hex').slice(0, 20);

espIndex = espIndex
  .replaceAll('./assets/app.js', `./assets/app.js?v=${buildId}`)
  .replaceAll('./assets/app.css', `./assets/app.css?v=${buildId}`)
  .replace(
    '</head>',
    `<meta name=\"geolocator-webui-build\" content=\"${buildId}\"></head>`,
  );

const gzipOptions = { level: 9, mtime: 0 };
const jsGzip = gzipSync(sourceJs, gzipOptions);
const cssGzip = gzipSync(sourceCss, gzipOptions);
const secondaryGzip = secondaryAssets.map((asset) => ({
  name: asset.name,
  data: gzipSync(asset.data, gzipOptions),
}));

function cString(value) {
  return JSON.stringify(value);
}
function embeddedSymbolBase(fileName) {
  const embeddedPath = `generated_webui_chunks/${fileName}.gz`;
  return `_binary_${embeddedPath.replace(/[^A-Za-z0-9_]/g, '_')}`;
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
    `${cString('application/javascript; charset=utf-8')}, ${alias}_start, ${alias}_end },`,
  );
});
const generatedTable = `${declarations.join('\n')}\n\n` +
  `static const web_webui_chunk_asset_t s_webui_chunk_assets[] = {\n${tableRows.join('\n')}\n};\n` +
  `#define WEB_WEBUI_CHUNK_ASSET_COUNT (sizeof(s_webui_chunk_assets) / sizeof(s_webui_chunk_assets[0]))\n`;

await rm(espDir, { recursive: true, force: true });
await mkdir(espAssetsDir, { recursive: true });

await Promise.all([
  writeFile(join(espDir, 'index.html'), espIndex, 'utf8'),
  writeFile(join(espDir, 'build-id.txt'), `${buildId}\n`, 'utf8'),
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
  console.log(`  assets/${asset.name}.gz ${asset.data.length} bytes`);
}
console.log(`  secondary chunks  ${secondaryGzip.length} files / ${secondaryTotal} gzip bytes`);
""",
)

# 5) Shared metadata for generated secondary chunks.
replace_once(
    "components/web_server/web_server_internal.h",
    """typedef enum
{
    WEB_STATIC_ASSET_INDEX_HTML = 0,""",
    """typedef struct
{
    const char *uri;
    const char *sd_suffix;
    const char *content_type;
    const unsigned char *start;
    const unsigned char *end;

} web_webui_chunk_asset_t;


typedef enum
{
    WEB_STATIC_ASSET_INDEX_HTML = 0,""",
)

# 6) Preserve SD seed completeness even though runtime React delivery stays in
# flash. This keeps the pre-existing maintenance/fallback bundle internally
# coherent with the split module graph.
replace_once(
    "components/web_server/web_webui.c",
    """extern const unsigned char webui_css_gz_end[]
    asm(\"_binary_generated_webui_assets_app_css_gz_end\");
#endif
""",
    """extern const unsigned char webui_css_gz_end[]
    asm(\"_binary_generated_webui_assets_app_css_gz_end\");

#include \"generated_webui_chunk_table.inc\"
#endif
""",
)
replace_once(
    "components/web_server/web_webui.c",
    """    for (
        size_t index = 0;
        index < sizeof(required_suffixes) / sizeof(required_suffixes[0]);
        ++index
    ) {
        if (
            !web_webui_build_path(
                path,
                sizeof(path),
                required_suffixes[index]
            ) ||
            !web_webui_file_exists(
                path
            )
        ) {
            return false;
        }
    }

    return true;
}""",
    """    for (
        size_t index = 0;
        index < sizeof(required_suffixes) / sizeof(required_suffixes[0]);
        ++index
    ) {
        if (
            !web_webui_build_path(
                path,
                sizeof(path),
                required_suffixes[index]
            ) ||
            !web_webui_file_exists(
                path
            )
        ) {
            return false;
        }
    }

#if WEB_SERVER_WEBUI_SEED_EMBEDDED
    for (size_t index = 0; index < WEB_WEBUI_CHUNK_ASSET_COUNT; ++index) {
        if (
            !web_webui_build_path(
                path,
                sizeof(path),
                s_webui_chunk_assets[index].sd_suffix
            ) ||
            !web_webui_file_exists(path)
        ) {
            return false;
        }
    }
#endif

    return true;
}""",
)
replace_once(
    "components/web_server/web_webui.c",
    """    err =
        web_webui_write_atomic(
            path,
            webui_css_gz_start,
            (size_t)(
                webui_css_gz_end -
                webui_css_gz_start
            )
        );

    if (err != ESP_OK) {
        return err;
    }

    if (
        !web_webui_build_path(
            path,
            sizeof(path),
            \"/build-id.txt\"
        )
    ) {""",
    """    err =
        web_webui_write_atomic(
            path,
            webui_css_gz_start,
            (size_t)(
                webui_css_gz_end -
                webui_css_gz_start
            )
        );

    if (err != ESP_OK) {
        return err;
    }

    for (size_t index = 0; index < WEB_WEBUI_CHUNK_ASSET_COUNT; ++index) {
        const web_webui_chunk_asset_t *asset = &s_webui_chunk_assets[index];
        if (
            !web_webui_build_path(
                path,
                sizeof(path),
                asset->sd_suffix
            )
        ) {
            return ESP_ERR_INVALID_SIZE;
        }

        err = web_webui_write_atomic(
            path,
            asset->start,
            (size_t)(asset->end - asset->start)
        );
        if (err != ESP_OK) {
            return err;
        }
    }

    if (
        !web_webui_build_path(
            path,
            sizeof(path),
            \"/build-id.txt\"
        )
    ) {""",
)

# 7) Runtime flash delivery: one wildcard allow-list route replaces the previous
# two fixed handlers, so arbitrary content-hashed chunks do not consume URI
# handler slots and no filesystem path from the request is ever opened.
write(
    "components/web_server/web_webui_flash.c",
    """#include \"web_server_internal.h\"

#include <stddef.h>
#include <string.h>

#include \"esp_log.h\"


#define WEB_WEBUI_FLASH_CHUNK_BYTES  1024U


static const char *TAG =
    \"WEB_WEBUI_FLASH\";


#if WEB_SERVER_WEBUI_SEED_EMBEDDED
extern const unsigned char webui_index_start[]
    asm(\"_binary_generated_webui_index_html_start\");
extern const unsigned char webui_index_end[]
    asm(\"_binary_generated_webui_index_html_end\");

extern const unsigned char webui_js_gz_start[]
    asm(\"_binary_generated_webui_assets_app_js_gz_start\");
extern const unsigned char webui_js_gz_end[]
    asm(\"_binary_generated_webui_assets_app_js_gz_end\");

extern const unsigned char webui_css_gz_start[]
    asm(\"_binary_generated_webui_assets_app_css_gz_start\");
extern const unsigned char webui_css_gz_end[]
    asm(\"_binary_generated_webui_assets_app_css_gz_end\");

#include \"generated_webui_chunk_table.inc\"
#endif


static esp_err_t web_webui_flash_send(
    httpd_req_t *request,
    const unsigned char *start,
    const unsigned char *end,
    const char *content_type,
    bool gzip_encoded,
    bool text_file
)
{
    if (
        request == NULL ||
        start == NULL ||
        end == NULL ||
        end < start ||
        content_type == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_resp_set_type(request, content_type);
    httpd_resp_set_hdr(request, \"Cache-Control\", \"no-store\");
    if (gzip_encoded) {
        httpd_resp_set_hdr(request, \"Content-Encoding\", \"gzip\");
    }

    const size_t length =
        text_file
            ? strlen((const char *)start)
            : (size_t)(end - start);

    size_t offset = 0;
    while (offset < length) {
        const size_t remaining = length - offset;
        const size_t chunk_length =
            remaining > WEB_WEBUI_FLASH_CHUNK_BYTES
                ? WEB_WEBUI_FLASH_CHUNK_BYTES
                : remaining;
        const esp_err_t err = httpd_resp_send_chunk(
            request,
            (const char *)start + offset,
            chunk_length
        );
        if (err != ESP_OK) {
            return err;
        }
        offset += chunk_length;
    }

    return httpd_resp_send_chunk(request, NULL, 0);
}


static bool web_webui_uri_equals(
    const char *request_uri,
    const char *expected
)
{
    if (request_uri == NULL || expected == NULL) {
        return false;
    }
    const char *query = strchr(request_uri, '?');
    const size_t request_length =
        query == NULL
            ? strlen(request_uri)
            : (size_t)(query - request_uri);
    const size_t expected_length = strlen(expected);
    return
        request_length == expected_length &&
        strncmp(request_uri, expected, request_length) == 0;
}


esp_err_t web_webui_flash_send_index(
    httpd_req_t *request
)
{
#if WEB_SERVER_WEBUI_SEED_EMBEDDED
    ESP_LOGI(TAG, \"React index served from flash\");
    return web_webui_flash_send(
        request,
        webui_index_start,
        webui_index_end,
        \"text/html; charset=utf-8\",
        false,
        true
    );
#else
    (void)request;
    return ESP_ERR_NOT_FOUND;
#endif
}


static esp_err_t web_webui_flash_asset_handler(
    httpd_req_t *request
)
{
#if WEB_SERVER_WEBUI_SEED_EMBEDDED
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (web_webui_uri_equals(request->uri, \"/assets/app.js\")) {
        return web_webui_flash_send(
            request,
            webui_js_gz_start,
            webui_js_gz_end,
            \"application/javascript; charset=utf-8\",
            true,
            false
        );
    }
    if (web_webui_uri_equals(request->uri, \"/assets/app.css\")) {
        return web_webui_flash_send(
            request,
            webui_css_gz_start,
            webui_css_gz_end,
            \"text/css; charset=utf-8\",
            true,
            false
        );
    }

    for (size_t index = 0; index < WEB_WEBUI_CHUNK_ASSET_COUNT; ++index) {
        const web_webui_chunk_asset_t *asset = &s_webui_chunk_assets[index];
        if (!web_webui_uri_equals(request->uri, asset->uri)) {
            continue;
        }
        ESP_LOGI(TAG, \"React chunk served from flash: %s\", asset->uri);
        return web_webui_flash_send(
            request,
            asset->start,
            asset->end,
            asset->content_type,
            true,
            false
        );
    }

    return httpd_resp_send_err(
        request,
        HTTPD_404_NOT_FOUND,
        \"React Web UI asset unavailable\"
    );
#else
    (void)request;
    return ESP_ERR_NOT_FOUND;
#endif
}


esp_err_t web_webui_flash_register(
    httpd_handle_t server
)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if WEB_SERVER_WEBUI_SEED_EMBEDDED
    const httpd_uri_t asset_uri = {
        .uri = \"/assets/*\",
        .method = HTTP_GET,
        .handler = web_webui_flash_asset_handler,
        .user_ctx = NULL,
    };
    return httpd_register_uri_handler(server, &asset_uri);
#else
    return ESP_OK;
#endif
}
""",
)

# 8) Enable wildcard URI matching only on the secure server where React assets
# are registered. Exact routes continue to match literally under this matcher.
replace_once(
    "components/web_server/web_server.c",
    """    config.httpd.max_uri_handlers =
        14;
""",
    """    config.httpd.max_uri_handlers =
        14;

    config.httpd.uri_match_fn =
        httpd_uri_match_wildcard;
""",
)

# 9) CMake: copy and embed every secondary gzip chunk, plus the generated table
# include. app.js/app.css keep their existing generated paths/symbols.
replace_once(
    "components/web_server/CMakeLists.txt",
    """set(WEB_SERVER_WEBUI_GENERATED_CSS
    \"${CMAKE_CURRENT_LIST_DIR}/generated_webui_assets_app.css.gz\"
)
""",
    """set(WEB_SERVER_WEBUI_GENERATED_CSS
    \"${CMAKE_CURRENT_LIST_DIR}/generated_webui_assets_app.css.gz\"
)

set(WEB_SERVER_WEBUI_GENERATED_CHUNK_DIR
    \"${CMAKE_CURRENT_LIST_DIR}/generated_webui_chunks\"
)

set(WEB_SERVER_WEBUI_GENERATED_CHUNK_TABLE
    \"${CMAKE_CURRENT_LIST_DIR}/generated_webui_chunk_table.inc\"
)
""",
)
replace_once(
    "components/web_server/CMakeLists.txt",
    """    file(REMOVE_RECURSE
        \"${CMAKE_CURRENT_LIST_DIR}/generated_webui\"
    )

    if(
        EXISTS \"${WEB_SERVER_WEBUI_SOURCE_DIR}/index.html\"
        AND EXISTS \"${WEB_SERVER_WEBUI_SOURCE_DIR}/build-id.txt\"
        AND EXISTS \"${WEB_SERVER_WEBUI_SOURCE_DIR}/assets/app.js.gz\"
        AND EXISTS \"${WEB_SERVER_WEBUI_SOURCE_DIR}/assets/app.css.gz\"
    )""",
    """    file(REMOVE_RECURSE
        \"${CMAKE_CURRENT_LIST_DIR}/generated_webui\"
        \"${WEB_SERVER_WEBUI_GENERATED_CHUNK_DIR}\"
    )
    file(REMOVE
        \"${WEB_SERVER_WEBUI_GENERATED_CHUNK_TABLE}\"
    )

    if(
        EXISTS \"${WEB_SERVER_WEBUI_SOURCE_DIR}/index.html\"
        AND EXISTS \"${WEB_SERVER_WEBUI_SOURCE_DIR}/build-id.txt\"
        AND EXISTS \"${WEB_SERVER_WEBUI_SOURCE_DIR}/generated_webui_chunk_table.inc\"
        AND EXISTS \"${WEB_SERVER_WEBUI_SOURCE_DIR}/assets/app.js.gz\"
        AND EXISTS \"${WEB_SERVER_WEBUI_SOURCE_DIR}/assets/app.css.gz\"
    )""",
)
replace_once(
    "components/web_server/CMakeLists.txt",
    """        configure_file(
            \"${WEB_SERVER_WEBUI_SOURCE_DIR}/assets/app.css.gz\"
            \"${WEB_SERVER_WEBUI_GENERATED_CSS}\"
            COPYONLY
        )

        file(READ""",
    """        configure_file(
            \"${WEB_SERVER_WEBUI_SOURCE_DIR}/assets/app.css.gz\"
            \"${WEB_SERVER_WEBUI_GENERATED_CSS}\"
            COPYONLY
        )

        configure_file(
            \"${WEB_SERVER_WEBUI_SOURCE_DIR}/generated_webui_chunk_table.inc\"
            \"${WEB_SERVER_WEBUI_GENERATED_CHUNK_TABLE}\"
            COPYONLY
        )

        file(MAKE_DIRECTORY
            \"${WEB_SERVER_WEBUI_GENERATED_CHUNK_DIR}\"
        )
        file(GLOB WEB_SERVER_WEBUI_CHUNK_SOURCES
            \"${WEB_SERVER_WEBUI_SOURCE_DIR}/assets/*.js.gz\"
        )
        list(SORT WEB_SERVER_WEBUI_CHUNK_SOURCES)
        set(WEB_SERVER_WEBUI_CHUNK_COUNT 0)
        foreach(WEB_SERVER_WEBUI_CHUNK_SOURCE IN LISTS WEB_SERVER_WEBUI_CHUNK_SOURCES)
            get_filename_component(
                WEB_SERVER_WEBUI_CHUNK_NAME
                \"${WEB_SERVER_WEBUI_CHUNK_SOURCE}\"
                NAME
            )
            if(WEB_SERVER_WEBUI_CHUNK_NAME STREQUAL \"app.js.gz\")
                continue()
            endif()
            math(EXPR WEB_SERVER_WEBUI_CHUNK_COUNT \"${WEB_SERVER_WEBUI_CHUNK_COUNT} + 1\")
            configure_file(
                \"${WEB_SERVER_WEBUI_CHUNK_SOURCE}\"
                \"${WEB_SERVER_WEBUI_GENERATED_CHUNK_DIR}/${WEB_SERVER_WEBUI_CHUNK_NAME}\"
                COPYONLY
            )
            list(APPEND WEB_SERVER_WEBUI_EMBED_FILES
                \"generated_webui_chunks/${WEB_SERVER_WEBUI_CHUNK_NAME}\"
            )
        endforeach()
        if(WEB_SERVER_WEBUI_CHUNK_COUNT EQUAL 0)
            message(FATAL_ERROR
                \"GeoLocator WebUI split build produced no secondary JS chunks\"
            )
        endif()

        file(READ""",
)

print("XR code-splitting patch applied")
