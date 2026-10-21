import { defineConfig, devices } from '@playwright/test';

export default defineConfig({
  testDir: './tests/e2e',
  testMatch: 'navigation.spec.ts',
  forbidOnly: true,
  fullyParallel: false,
  workers: 1,
  retries: 0,
  timeout: 30_000,
  expect: { timeout: 7_000 },
  updateSnapshots: 'none',
  outputDir: '../.test-build/ui-web/results',
  reporter: [
    ['line'],
    ['json', { outputFile: '../.test-build/ui-web/playwright.json' }],
    ['html', { outputFolder: '../.test-build/ui-web/html', open: 'never' }],
  ],
  metadata: {
    evidence: 'Production WebUI; synthetic HTTP; Android Chrome emulation, no physical XR',
    selection: 'Five sections EN/UK, API failure visibility, runtime/network guards',
    viewport: '393x852',
    expectedTests: 4,
  },
  use: {
    baseURL: 'http://127.0.0.1:4179',
    serviceWorkers: 'block',
    trace: 'retain-on-failure',
    screenshot: 'only-on-failure',
    video: 'off',
  },
  projects: [{
    name: 'android-chromium-smoke',
    use: { ...devices['Pixel 5'], viewport: { width: 393, height: 852 } },
  }],
  webServer: {
    command: 'npm run build && node node_modules/vite/bin/vite.js preview --host 127.0.0.1 --port 4179 --strictPort',
    url: 'http://127.0.0.1:4179',
    reuseExistingServer: false,
    timeout: 120_000,
    stdout: 'ignore',
    stderr: 'pipe',
  },
});
