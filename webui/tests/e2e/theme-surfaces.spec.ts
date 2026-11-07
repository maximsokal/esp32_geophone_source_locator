import { test, expect } from './fixtures/device';
import { openSection } from './helpers/layout';

/*
 * S03 (RED_DESIGN): main DOM surfaces use the LVGL roles of
 * components/ui/ui_theme.h. Old values: body #050a12 + radial gradient,
 * .console-card rgba(13,29,40,.92) / #203b4d / 1rem radius, shell #071018 gradient.
 */
for (const locale of ['en', 'uk'] as const) {
  test.describe(`theme surfaces ${locale}`, () => {
    test.use({ appLocale: locale });

    test('every full-page surface is flat BACKGROUND', async ({ page }) => {
      await page.goto('/');
      await openSection(page, 'home', locale);
      for (const selector of ['html', 'body', '#root', '.device-console-shell']) {
        const el = page.locator(selector);
        await expect(el).toHaveCount(1);
        await expect(el).toHaveCSS('background-color', 'rgb(8, 17, 31)');
        await expect(el).toHaveCSS('background-image', 'none');
      }
      await expect(page.locator('body')).toHaveCSS('color', 'rgb(243, 247, 251)');
    });

    test('console card uses PANEL, PANEL_ALT border and 7px radius', async ({ page }) => {
      await page.goto('/');
      await openSection(page, 'home', locale);
      const card = page.locator('.console-card').first();
      await expect(card).toBeVisible();
      await expect(card).toHaveCSS('background-color', 'rgb(19, 34, 56)');
      await expect(card).toHaveCSS('border-color', 'rgb(27, 45, 70)');
      await expect(card).toHaveCSS('border-width', '1px');
      await expect(card).toHaveCSS('border-radius', '7px');
    });

    test('buttons keep 44px targets and a visible focus ring', async ({ page }) => {
      await page.goto('/');
      await openSection(page, 'home', locale);
      /* Topbar navigation is compact by design (MAX-383). */
      const buttons = page.locator('button:visible:enabled:not(.console-topbar nav button):not(.brand-button)');
      const count = await buttons.count();
      expect(count).toBeGreaterThan(0);
      for (let i = 0; i < count; i++) {
        await expect(buttons.nth(i)).toHaveCSS('min-width', '44px');
        await expect(buttons.nth(i)).toHaveCSS('min-height', '44px');
      }
      await page.keyboard.press('Tab');
      const focused = page.locator(':focus');
      await expect(focused).toBeVisible();
      await expect(focused).toHaveCSS('outline-style', 'solid');
      await expect(focused).toHaveCSS('outline-width', '2px');
    });
  });
}
