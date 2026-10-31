import { defineConfig } from '@playwright/test';
import base from './playwright.config';

export default defineConfig(base, {
  testMatch: ['mobile-layout.spec.ts', 'accessibility.spec.ts', 'negative-controls.spec.ts', 'scanner-style.spec.ts'],
  timeout: 60_000,
  outputDir: '../.test-build/ui-layout/results',
  reporter: [
    ['line'],
    ['json', { outputFile: '../.test-build/ui-layout/playwright.json' }],
    ['html', { outputFolder: '../.test-build/ui-layout/html', open: 'never' }],
  ],
  metadata: {
    ...base.metadata,
    selection: 'Seven sizes EN/UK, all workflow stages, 200% text, axe and detector controls',
    viewport: '320x568,360x800,393x852,412x915,430x932,844x390,768x1024',
    expectedTests: 85,
    reproduce: 'cd webui; npm run test:ui:layout',
  },
});
