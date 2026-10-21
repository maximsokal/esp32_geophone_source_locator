import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';

export default defineConfig({
  plugins: [react()],
  test: {
    include: ['tests/components/**/*.dom.test.tsx'],
    environment: 'jsdom',
    environmentOptions: { jsdom: { url: 'http://localhost/', pretendToBeVisual: true } },
    setupFiles: ['./tests/components/setup.ts'],
    allowOnly: false,
    retry: 0,
    testTimeout: 10_000,
    hookTimeout: 10_000,
    restoreMocks: true,
  },
});
