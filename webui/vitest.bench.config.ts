import { defineConfig } from 'vitest/config';

/* Timing benchmarks (`npm run bench:live-mesh`): not part of the default suites, because wall-clock numbers belong to the machine they ran on. */
export default defineConfig({
  server: { fs: { allow: ['..'] } },
  test: {
    include: ['tests/bench/**/*.bench.ts'],
    allowOnly: false,
    retry: 0,
    testTimeout: 300_000,
    fileParallelism: false,
  },
});
