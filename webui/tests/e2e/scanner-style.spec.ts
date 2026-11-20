import { test, expect, catalog } from './fixtures/device';
import { installJourneyXr, journeyXrState } from './fixtures/xr-session';
import { scenarioScreenshot } from './fixtures/scenarios';
import { assertReachable } from './helpers/layout';

for (const locale of ['en', 'uk'] as const) {
  test.describe(`active scanner CSS ${locale}`, () => {
    test.use({ appLocale: locale });
    for (const [width, height] of [[320, 568], [393, 852], [844, 390], [768, 1024]]) {
      test(`opaque ${width}x${height} and enlarged text`, async ({ page }) => {
        const t = catalog[locale];
        await page.setViewportSize({ width, height });
        await installJourneyXr(page);
        await page.goto('/');
        await page.locator('[data-command="SCAN_START"]').click();
        await expect(page.locator('.xr-start-button')).toBeEnabled();
        await page.locator('.xr-start-button').click();
        await expect.poll(() => journeyXrState(page)).toEqual({ requests: 1, active: true });
        for (const enlarged of [false, true]) {
          if (enlarged) await page.addStyleTag({ content: 'html { font-size: 200%; }' });
          for (const selector of ['html', 'body', '#root']) {
            await expect(page.locator(selector), 'opaque XR must not expose the browser white canvas').toHaveCSS('background-color', 'rgb(8, 17, 31)');
          }
          const geometry = await page.evaluate(() => {
            const card = document.querySelector('.xr-overlay-card')!;
            const box = card.getBoundingClientRect();
            const back = document.querySelector('.console-floating-back')!.getBoundingClientRect();
            return { top: box.top, bottom: box.bottom, left: box.left, right: box.right,
              backBottom: back.bottom,
              width: innerWidth, height: innerHeight, overflow: getComputedStyle(card).overflowY,
              pageWidth: document.documentElement.scrollWidth };
          });
          expect(geometry.top, 'scanner controls must stay below the Back button').toBeGreaterThanOrEqual(64);
          expect(geometry.top - geometry.backBottom, 'Back must not overlap the panel at enlarged text sizes').toBeGreaterThanOrEqual(8);
          expect(geometry.bottom).toBeLessThanOrEqual(geometry.height);
          expect(geometry.left).toBeGreaterThanOrEqual(0);
          expect(geometry.right).toBeLessThanOrEqual(geometry.width);
          expect(geometry.pageWidth).toBeLessThanOrEqual(geometry.width);
          expect(geometry.overflow, 'short viewports must allow controls to scroll').toBe('auto');
          await expect(page.locator('.xr-finish-button')).toHaveCSS('background-color', 'rgb(153, 27, 27)');
          await expect(page.locator('.xr-overlay-card button').first()).toHaveCSS('color', 'rgb(243, 247, 251)');
          const clipped = await page.locator('.xr-overlay-card button').evaluateAll(buttons => buttons.filter(button => {
            const rect = button.getBoundingClientRect();
            return rect.width < 44 || rect.height < 44 || button.scrollWidth > button.clientWidth + 1 || button.scrollHeight > button.clientHeight + 1;
          }).map(button => button.textContent));
          expect(clipped, 'scanner buttons need readable labels and touch-sized targets').toEqual([]);
          await assertReachable(page);
          await scenarioScreenshot(page, `${locale}-active-${width}x${height}-${enlarged ? '200pct' : 'normal'}`);
        }
        await page.locator('.xr-finish-button').click();
        await expect(page.locator('html')).not.toHaveClass(/xr-session-active|xr-passthrough-active/);
        await expect(page.locator('.xr-immersive-overlay')).toBeHidden();
        await expect(page.locator('.xr-start-button')).toBeEnabled();
        await page.getByRole('button', { name: t['console.back_object'], exact: true }).click();
        await expect(page.locator('.workflow-page')).toBeVisible();
        await expect(page.locator('.device-console-shell')).toHaveCSS('background-color', 'rgb(8, 17, 31)');
        await scenarioScreenshot(page, `${locale}-restored-${width}x${height}`);
      });
    }
    for (const blend of ['alpha-blend', 'additive'] as const) {
      test(`${blend} preserves camera transparency and restores theme`, async ({ page }) => {
        await installJourneyXr(page, blend);
        await page.goto('/');
        await page.locator('[data-command="SCAN_START"]').click();
        await expect(page.locator('.xr-start-button')).toBeEnabled();
        await page.locator('.xr-start-button').click();
        await expect.poll(() => journeyXrState(page)).toEqual({ requests: 1, active: true });
        await expect(page.locator('html')).toHaveClass(/xr-passthrough-active/);
        for (const selector of ['html', 'body', '#root']) {
          await expect(page.locator(selector)).toHaveCSS('background-color', 'rgba(0, 0, 0, 0)');
        }
        await page.locator('.xr-finish-button').click();
        await expect(page.locator('html')).not.toHaveClass(/xr-session-active|xr-passthrough-active/);
        await expect(page.locator('.xr-start-button')).toBeEnabled();
      });
    }
  });
}
