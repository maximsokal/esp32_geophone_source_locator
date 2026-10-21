import { test as base, expect } from '@playwright/test';
import { en } from '../../../src/i18n/catalog.en.gen';
import { uk } from '../../../src/i18n/catalog.uk.gen';
import { DeviceApi } from './api';

export const test = base.extend<{ api: DeviceApi; appLocale: 'en' | 'uk' }>({
  appLocale: ['en', { option: true }],
  api: async ({ context, baseURL }, use) => {
    const api = new DeviceApi();
    await api.install(context, new URL(baseURL!).origin);
    await use(api);
  },
  page: async ({ page, api, appLocale, browser }, use, testInfo) => {
    const errors: string[] = [];
    page.on('pageerror', (error) => errors.push(`Uncaught: ${error.message}`));
    page.on('console', (message) => {
      if (message.type() !== 'error') return;
      const location = message.location().url;
      const expectedResourceError = location && api.expectedHttpErrors.has(new URL(location).pathname) &&
        message.text().startsWith('Failed to load resource:');
      if (!expectedResourceError) errors.push(`Console: ${message.text()}`);
    });
    page.on('response', (response) => {
      if (response.status() >= 400 && !api.expectedHttpErrors.has(new URL(response.url()).pathname)) {
        errors.push(`HTTP ${response.status()}: ${response.url()}`);
      }
    });
    await page.addInitScript((locale) => {
      localStorage.setItem('geolocator.locale', JSON.stringify({ schema: 1, locale }));
    }, appLocale);
    await page.clock.setFixedTime(new Date('2026-01-01T12:00:00Z'));
    await use(page);
    await testInfo.attach('runtime', { contentType: 'application/json', body: JSON.stringify({
      browser: browser.version(), locale: appLocale, viewport: page.viewportSize(),
      requests: api.requests, errors, blocked: api.problems,
    }, null, 2) });
    expect([...api.problems, ...errors], 'No unexpected network, console or page errors').toEqual([]);
  },
});

export { expect };
export const catalog = { en, uk };
