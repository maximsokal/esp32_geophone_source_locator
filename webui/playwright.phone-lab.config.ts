import { defineConfig, devices } from '@playwright/test';

/*
 * Phone Lab smoke proof on loopback: the REAL app served by the phone-lab dev server (fake ESP32 API + telemetry), opened in
 * Chromium. A physical phone, WebXR and the LAN path are NOT covered here; they are verified by the person holding the phone.
 */
export default defineConfig({
  testDir: './tests/phone-lab',
  testMatch: 'phone-lab.spec.ts',
  forbidOnly: true,
  fullyParallel: false,
  workers: 1,
  retries: 0,
  timeout: 90_000,
  expect: { timeout: 20_000 },
  outputDir: '../.test-build/phone-lab-e2e/results',
  reporter: [['line'], ['json', { outputFile: '../.test-build/phone-lab-e2e/playwright.json' }]],
  use: { baseURL: 'https://127.0.0.1:5443', ignoreHTTPSErrors: true, serviceWorkers: 'block', trace: 'retain-on-failure', screenshot: 'only-on-failure' },
  projects: [{ name: 'phone-lab-chromium', use: { ...devices['Pixel 5'], viewport: { width: 393, height: 852 } } }],
  webServer: {
    command: 'node scripts/run-phone-lab.mjs',
    url: 'https://127.0.0.1:5443/lab/health',
    ignoreHTTPSErrors: true,
    reuseExistingServer: true,
    timeout: 90_000,
    env: { PHONE_LAB_SELFSIGNED: '1' },
    stdout: 'pipe',
    stderr: 'pipe',
  },
});
