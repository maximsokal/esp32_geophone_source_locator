import { defineConfig } from '@playwright/test';
import graphics from './playwright.graphics.config';

/* Framebuffer pixel tests of the live mesh renderer: the REAL renderer on a native WebGL2 context (SwiftShader), pixels read back. */
export default defineConfig({ ...graphics,
  testMatch: 'live-mesh-pixels.spec.ts',
  reporter: [['line'], ['json', { outputFile: '../.test-build/ui-live-mesh/playwright.json' }]],
  outputDir: '../.test-build/ui-live-mesh/results',
  metadata: { ...graphics.metadata, selection: 'Live mesh framebuffer pixels (single-layer opacity, overlap, AprilTag contrast, state restoration, occlusion)', expectedTests: 16 },
});
