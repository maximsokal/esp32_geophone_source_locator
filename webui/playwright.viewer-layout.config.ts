import { defineConfig } from '@playwright/test';
import graphics from './playwright.graphics.config';

/* Bounding-rectangle layout tests of the final 3D viewer at seven widths, portrait, landscape and full screen (TASK 07), on the real viewer (SwiftShader). */
export default defineConfig({ ...graphics,
  testMatch: 'viewer-layout.spec.ts',
  reporter: [['line'], ['json', { outputFile: '../.test-build/ui-viewer-layout/playwright.json' }]],
  outputDir: '../.test-build/ui-viewer-layout/results',
  use: { ...graphics.use, screenshot: 'only-on-failure' },
  metadata: { ...graphics.metadata, selection: 'Viewer controls vs hints: bounding rectangles at 320/360/390/412/480/768/1280 portrait, 5 landscape sizes, full screen, EN/UK', expectedTests: 40, reproduce: 'cd webui; npm run test:ui:viewer-layout' },
});
