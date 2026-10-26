import { defineConfig, devices } from '@playwright/test';

/*
 * The 320x240 TFT is rendered by the real LVGL on a native host
 * (python tools/run_ui_render_tests.py) into a browsable gallery. These tests
 * open that gallery in Chromium; they do not drive the physical display.
 */
export default defineConfig({
  testDir: './tests/e2e',
  testMatch: 'tft-sensor-values.spec.ts',
  forbidOnly: true,
  fullyParallel: false,
  workers: 1,
  retries: 0,
  timeout: 30_000,
  expect: { timeout: 7_000 },
  updateSnapshots: 'none',
  outputDir: '../.test-build/ui-tft/results',
  reporter: [
    ['line'],
    ['json', { outputFile: '../.test-build/ui-tft/playwright.json' }],
    ['html', { outputFolder: '../.test-build/ui-tft/html', open: 'never' }],
  ],
  metadata: {
    evidence: 'Real LVGL 9 render of the production sensor-check screen on a native host; synthetic sensor data; no physical TFT',
    selection: 'Pressure/temperature/gyroscope values page of the G1-G4 cards, English and Ukrainian, full and partial data',
    reproduce: 'cd webui; npm run test:ui:tft',
  },
  use: { trace: 'retain-on-failure', screenshot: 'only-on-failure', video: 'off' },
  projects: [{ name: 'tft-gallery-chromium', use: { ...devices['Desktop Chrome'], viewport: { width: 1280, height: 900 } } }],
});
