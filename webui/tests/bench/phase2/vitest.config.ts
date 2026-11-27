import { resolve } from 'node:path';
import { defineConfig } from 'vitest/config';

/* Phase 2 benchmark runner: `npm run bench:phase2` (or --config tests/bench/phase2/vitest.config.ts). It lives beside the benches so that it exists at every checkout. */
export default defineConfig({
  root: resolve(__dirname, '../../..'),
  server: { fs: { allow: [resolve(__dirname, '../../../..'), 'E:/datasets'] } },
  test: { include: ['tests/bench/phase2/**/*.bench.ts'], allowOnly: false, retry: 0, testTimeout: 1_800_000, fileParallelism: false },
});
