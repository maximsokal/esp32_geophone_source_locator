import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';

export default defineConfig({
  plugins: [react()],
  /* tests/components/theme.drift reads components/ui/ui_theme.h, one level above webui. */
  server: { fs: { allow: ['..'] } },
  test: {
    include: ['tests/components/**/*.dom.test.tsx'],
    /* Process theme.css so `?raw` returns its text instead of vitest's empty CSS stub. */
    css: { include: [/theme\.css/] },
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
