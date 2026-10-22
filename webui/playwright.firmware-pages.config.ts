import { defineConfig } from '@playwright/test';
import base from './playwright.config';

export default defineConfig({ ...base, webServer: undefined }, {
  testMatch: 'firmware-pages.spec.ts',
  use: { ...base.use, baseURL: 'https://geolocator.test', screenshot: { mode: 'on', fullPage: true } },
  outputDir: '../.test-build/ui-firmware-pages/results',
  reporter: [['line'], ['json', { outputFile: '../.test-build/ui-firmware-pages/playwright.json' }],
    ['html', { outputFolder: '../.test-build/ui-firmware-pages/html', open: 'never' }]],
  metadata: { ...base.metadata, evidence: 'Captured production C HTML/assets; SDK, HTTP and phone capabilities simulated',
    selection: 'Firmware setup, onboarding, fallback and seismic pages; seven sizes, forms, errors and capability reports',
    expectedTests: 37, reproduce: 'python tools/render_web_pages.py; cd webui; npm run test:ui:firmware-pages' },
});
