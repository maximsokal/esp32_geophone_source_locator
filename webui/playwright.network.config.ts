import { defineConfig } from '@playwright/test';
import base from './playwright.config';

export default defineConfig(base, {
  testMatch: ['network.spec.ts', 'objects-workflow.spec.ts', 'seismic.spec.ts', 'workflow.spec.ts', 'workflow-journey.spec.ts', 'scanner-loading.spec.ts'],
  timeout: 45_000,
  use: { ...base.use, screenshot: { mode: 'on', fullPage: true } },
  outputDir: '../.test-build/ui-network/results',
  reporter: [['line'], ['json', { outputFile: '../.test-build/ui-network/playwright.json' }],
    ['html', { outputFolder: '../.test-build/ui-network/html', open: 'never' }]],
  metadata: { ...base.metadata, selection: 'HTTP faults/recovery, Objects, seismic, workflow and lazy-scanner lifecycle',
    expectedTests: 65, reproduce: 'cd webui; npm run test:ui:network' },
});
