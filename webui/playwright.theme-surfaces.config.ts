import { defineConfig } from '@playwright/test';
import base from './playwright.config';

export default defineConfig(base, {
  testMatch: ['theme-surfaces.spec.ts'],
  outputDir: '../.test-build/ui-theme-surfaces/results',
  reporter: [['line'], ['json', { outputFile: '../.test-build/ui-theme-surfaces/playwright.json' }]],
  metadata: { ...base.metadata, expectedTests: 6, reproduce: 'cd webui; npm run test:ui:theme-surfaces' },
});
