import { test, expect, catalog } from './fixtures/device';
import { scenarioScreenshot } from './fixtures/scenarios';
import { installSavedScan, openSavedSessionScan } from './fixtures/saved-scan';

for (const locale of ['en', 'uk'] as const) {
  test.describe(`scan information after the devices were scanned, ${locale}`, () => {
    test.use({ appLocale: locale });

    test('lists the recalculated pairs and the error reduction next to the 3D scan', async ({ page, api }, info) => {
      const t = catalog[locale];
      installSavedScan(api, true);
      await page.goto('/');
      await openSavedSessionScan(page, locale);
      const card = page.locator('.scan-refinement');
      await expect(card).toBeVisible({ timeout: 30_000 });
      await expect(card).toHaveAttribute('data-refinement-status', 'ok');
      await expect(card.getByText(t['scan.refine.title'], { exact: true })).toBeVisible();
      await expect(card.getByText('MAIN, G1, G2', { exact: true })).toBeVisible();
      for (const pair of ['MAIN-G1', 'MAIN-G2', 'G1-G2']) await expect(card.locator(`[data-pair="${pair}"]`)).toBeVisible();
      const percent = Number(await card.locator('[data-reduction-percent]').getAttribute('data-reduction-percent'));
      expect(percent).toBeGreaterThan(60);
      expect(percent).toBeLessThan(75);
      await expect(card.locator('.scan-refinement-model')).toHaveText(t['scan.refine.model']);
      /* The same screen holds the 3D canvas the devices are drawn in. */
      await expect(page.locator('.scan-webgl-canvas canvas')).toBeVisible();
      await scenarioScreenshot(page, `scan-distance-refinement-${locale}`);
      await info.attach('refinement-card', { body: await card.innerText(), contentType: 'text/plain' });
    });

    test('says that nothing was recalculated when no device was scanned', async ({ page, api }) => {
      const t = catalog[locale];
      installSavedScan(api, false);
      await page.goto('/');
      await openSavedSessionScan(page, locale);
      const card = page.locator('.scan-refinement');
      await expect(card).toBeVisible({ timeout: 30_000 });
      await expect(card).toHaveAttribute('data-refinement-status', 'unavailable');
      await expect(card.getByText(t['scan.refine.unavailable.no_registration'], { exact: true })).toBeVisible();
    });
  });
}
