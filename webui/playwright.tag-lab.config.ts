import { defineConfig, devices } from '@playwright/test';

/*
 * Tag Lab browser proof: real page, real detector Worker, real overlay canvas. The webcam project feeds a deterministic
 * y4m recording of an AprilTag through Chromium's fake capture device, so CI never needs a physical camera.
 */
export default defineConfig({
  testDir: './tests/tag-lab',
  testMatch: 'tag-lab.spec.ts',
  forbidOnly: true,
  fullyParallel: false,
  workers: 1,
  retries: 0,
  timeout: 60_000,
  expect: { timeout: 15_000 },
  outputDir: '../.test-build/tag-lab-e2e/results',
  reporter: [['line'], ['json', { outputFile: '../.test-build/tag-lab-e2e/playwright.json' }]],
  use: { baseURL: 'http://127.0.0.1:5188', serviceWorkers: 'block', trace: 'retain-on-failure', screenshot: 'only-on-failure' },
  projects: [{ name: 'tag-lab-chromium', use: { ...devices['Desktop Chrome'] } }],
  webServer: {
    command: 'node scripts/run-tag-lab.mjs',
    url: 'http://127.0.0.1:5188/tag-lab.html',
    reuseExistingServer: true,
    timeout: 60_000,
    stdout: 'pipe',
    stderr: 'pipe',
  },
});
