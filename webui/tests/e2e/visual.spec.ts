import { test as deviceTest, expect, catalog } from './fixtures/device';
import { prepareVisualState, visualStates } from './fixtures/visual-states';
import manifest from './snapshots/manifest.json' with { type: 'json' };

const test = deviceTest.extend<{ visualMode: 'capture' | 'compare' }>({ visualMode: ['compare', { option: true }] });
for (const state of visualStates) {
  test.describe(state.id, () => {
    test.use({ appLocale: state.locale });
    test(`visual ${state.id}`, { tag: `@${state.tier}` }, async ({ page, api, browser, visualMode }, info) => {
      expect(info.config.updateSnapshots, 'ordinary runs must never update references').toBe('none');
      expect(browser.version()).toBe(manifest.environment.browser);
      await page.addInitScript(({ cpu, memory }) => {
        Object.defineProperty(navigator, 'hardwareConcurrency', { get: () => cpu });
        Object.defineProperty(navigator, 'deviceMemory', { get: () => memory });
      }, { cpu: manifest.environment.hardwareConcurrency, memory: manifest.environment.deviceMemory });
      const file = `${state.id}.png`;
      try {
        await prepareVisualState(page, api, state);
        await expect(page.getByText(catalog[state.locale]['xr.value.checking'], { exact: false })).toHaveCount(0);
        await info.attach('visual-case', { body: JSON.stringify({ ...state, file }), contentType: 'application/json' });
        if (visualMode === 'compare') {
          // Compare before a diagnostic capture repaints the sticky backdrop's rounded edges.
          await expect(page).toHaveScreenshot(file, { fullPage: true });
          const errors = info.config.metadata.approvalErrors as Record<string, string>;
          expect(info.config.metadata.approvedFiles, 'reference must have explicit approval').toContain(file);
          expect(errors[file] ?? errors['*'], 'reference must match its approved hash').toBeUndefined();
        }
        const path = info.outputPath(file);
        await page.screenshot({ path, fullPage: true, animations: 'disabled', caret: 'hide', scale: 'css' });
        await info.attach('visual-actual', { path, contentType: 'image/png' });
      } finally {
        api.resumeStatus();
        if (state.variant === 'loading') await expect(page.locator('.device-dot')).toHaveClass(/online/);
      }
    });
  });
}
