import { defineConfig } from 'vitest/config';

/*
 * The MANDATORY local replay of the real scan of phone run 2026-11-26 23:15 (private room, never published): `npm run test:real-scan`.
 * It is not part of the default suites (its files end in .realscan.ts) and it never skips: when the original files are missing it FAILS, because a skipped
 * real-scan replay must not be mistaken for a passed one. The public CI relies on the synthetic suites (tests/xr/meshQuality.test.ts and others).
 */
export default defineConfig({
  server: { fs: { allow: ['..'] } },
  test: {
    include: ['tests/realscan/**/*.realscan.ts'],
    allowOnly: false,
    retry: 0,
    testTimeout: 300_000,
    hookTimeout: 60_000,
    fileParallelism: false,
  },
});
