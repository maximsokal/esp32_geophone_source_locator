import { test, expect, catalog } from './fixtures/device';

for (const locale of ['en', 'uk'] as const) {
  test.describe(locale, () => {
    test.use({ appLocale: locale });
    test('five sections and scanner return through real controls', async ({ page, api }) => {
      const t = catalog[locale];
      await page.goto('/');
      const nav = page.getByRole('navigation');
      await expect(nav.getByRole('button', { name: t['workflow.nav'], exact: true })).toHaveAttribute('aria-pressed', 'true');
      for (const [section, button, heading] of [
        ['home', t['nav.home'], t['dashboard.title']],
        ['objects', t['nav.objects'], 'Quarry'],
        ['sensors', t['nav.sensors'], t['sensors.acquisition_four_remotes']],
        ['workflow', t['workflow.nav'], t['workflow.title.scan_choice']],
      ]) {
        await test.step(section, async () => {
          await nav.getByRole('button', { name: button, exact: true }).click();
          await expect(page.getByRole('heading', { name: heading, exact: true })).toBeVisible();
          if (section === 'home') {
            await expect(page.locator('.hardware-state-badge')).toHaveText([
              t['web.hw.state.present'], t['web.hw.state.present'],
            ]);
          }
          if (section === 'sensors') {
            await expect.poll(() => api.requests.some((id) => id.startsWith('GET /api/nrf/logs?'))).toBe(true);
            await expect(page.getByRole('cell', { name: t['nrf_log.panel.no_records'], exact: true })).toBeVisible();
          }
          await page.evaluate(() => document.fonts.ready);
          const path = test.info().outputPath(`${locale}-${section}.png`);
          await page.screenshot({ path, fullPage: true });
          await test.info().attach(section, { path, contentType: 'image/png' });
        });
      }
      await page.getByRole('button', { name: t['workflow.action.scan_start'], exact: true }).click();
      await expect(page.getByRole('heading', { name: t['scan.search_volume_title'], exact: true })).toBeVisible();
      const path = test.info().outputPath(`${locale}-scanner.png`);
      await page.screenshot({ path, fullPage: true });
      await test.info().attach('scanner', { path, contentType: 'image/png' });
      await page.getByRole('button', { name: t['console.back_object'], exact: true }).click();
      await expect(page.getByRole('heading', { name: t['workflow.title.scan_choice'], exact: true })).toBeVisible();
      expect(api.requests.filter((id) => id === 'POST /api/control')).toHaveLength(2);
    });
  });
}

for (const failure of ['http', 'schema'] as const) {
  test(`status ${failure} failure is visible, never a successful link`, async ({ page, api }) => {
    api.statusFailure = failure;
    await page.goto('/');
    await expect(page.getByRole('alert')).toContainText(failure === 'http' ? '503' : 'Invalid');
    await expect(page.locator('.device-dot')).toHaveClass(/offline/);
    await expect(page.locator('.device-dot')).not.toHaveText(catalog.en['console.api_link']);
  });
}
