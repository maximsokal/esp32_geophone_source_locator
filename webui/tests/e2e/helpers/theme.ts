import { expect, type Page } from '@playwright/test';
import { contract } from './layout';

export async function assertSemanticStyles(page: Page) {
  for (const [role, selector] of Object.entries({
    primary: '.device-console-shell .console-actions button:not(.secondary):not(.danger),.device-console-shell .inline-form button:not(.secondary):not(.danger),.sensors-page .card-heading button:not(.secondary):not(.danger)',
    secondary: '.device-console-shell button.secondary', danger: '.device-console-shell button.danger',
    ready: '.state-pill.ready', offline: '.state-pill.offline', warning: '.console-alert.warning',
    error: '.console-alert[role=alert]:not(.warning),.console-error[role=alert]',
  })) {
    const expected = contract.theme[role as keyof typeof contract.theme];
    for (const el of await page.locator(selector).all()) {
      for (const [property, value] of Object.entries(expected)) await expect(el).toHaveCSS(property.replace(/[A-Z]/g, (c) => `-${c.toLowerCase()}`), value);
      if (await el.isDisabled()) {
        await expect(el).toHaveCSS('opacity', '0.45');
        await expect(el).toHaveCSS('cursor', 'not-allowed');
      }
    }
  }
  const locale = page.locator('.locale-picker select');
  if (await locale.count()) {
    await expect(locale).toHaveCSS('appearance', 'none');
    const values = await locale.evaluate((el) => {
      const s = getComputedStyle(el);
      return { padding: parseFloat(s.paddingRight), position: s.backgroundPositionY, image: s.backgroundImage };
    });
    expect(values.padding, 'native select reserves space for its centered arrow').toBeGreaterThanOrEqual(30);
    expect(values.position).toBe('50%');
    expect(values.image).not.toBe('none');
  }
}
