import { readFileSync } from 'node:fs';
import { defineConfig, mergeConfig, type Plugin } from 'vite';
import base from './vite.config';

/*
 * Mutation testing of the browser suites without touching a source file: with MUTATION_SPEC (a JSON file {path, original, replacement}) the one module is
 * changed inside this build only. A spec whose text is not found fails the build, so a mutation can never silently not apply.
 */
function mutationInMemory(): Plugin[] {
  const specPath = process.env.MUTATION_SPEC;
  if (specPath === undefined) return [];
  const spec = JSON.parse(readFileSync(specPath, 'utf8')) as { path: string; original: string; replacement: string };
  let applied = false;
  return [{
    name: 'in-memory-mutation', enforce: 'pre',
    transform(code, id) {
      if (!id.split('?')[0].replaceAll('\\', '/').endsWith('/' + spec.path)) return null;
      const source = code.replaceAll('\r\n', '\n');
      if (!source.includes(spec.original)) return null;
      applied = true;
      return { code: source.replace(spec.original, () => spec.replacement), map: null };
    },
    buildEnd() { if (!applied) this.error(`mutation not applied: ${spec.path}`); },
  }];
}

// Exercise emitted production modules/Workers without adding stress pages to ESP assets.
export default mergeConfig(base, defineConfig({
  plugins: mutationInMemory(),
  build: {
    outDir: '../.test-build/ui-graphics/bundle',
    rolldownOptions: {
      input: ['stress/scan-result-ui.html', 'stress/xr-mobile-stress.html', 'stress/xr-structural-stress.html', 'stress/live-mesh-pixels.html'],
      output: { entryFileNames: 'assets/[name]-[hash].js' },
    },
  },
}));
