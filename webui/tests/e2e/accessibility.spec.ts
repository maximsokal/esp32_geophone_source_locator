import AxeBuilder from '@axe-core/playwright';
import { test, expect } from './fixtures/device';
import { assertReachable, capture, openSection, sections } from './helpers/layout';

for (const locale of ['en', 'uk'] as const) {
  test.describe(locale, () => {
    test.use({ appLocale: locale });
    for (const section of sections) {
      test(`accessible ${section}`, async ({ page }) => {
        await page.goto('/');
        await openSection(page, section, locale);
        const result = await new AxeBuilder({ page }).withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa', 'wcag22aa']).analyze();
        await test.info().attach('axe', { body: JSON.stringify(result, null, 2), contentType: 'application/json' });
        await capture(page, `${locale}-accessible-${section}`);
        expect(result.violations.map(({ id, impact, nodes }) => ({ id, impact, nodes: nodes.map(({ target, failureSummary }) => ({ target, failureSummary })) }))).toEqual([]);
        await page.evaluate(() => { if (document.activeElement instanceof HTMLElement) document.activeElement.blur(); window.scrollTo(0, 0); });
        await page.keyboard.press('Tab');
        const focused = page.locator(':focus');
        await expect(focused).toBeVisible();
        await expect(focused).toHaveCSS('outline-style', 'solid');
        await expect(focused).toHaveCSS('outline-width', '2px');
        await assertReachable(page);
      });
    }
  });
}
