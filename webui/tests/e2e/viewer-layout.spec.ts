import { test, expect } from './fixtures/device';
import { installGraphicsProbe, rendered } from './helpers/graphics';
import type { Locator, Page } from '@playwright/test';

/*
 * TASK 07: the controls of the final 3D viewer (3D, Top, Front, Fit, Reset view, Pan, Focus, Full screen) must not cover the hint texts, nor each other,
 * and everything must stay inside the viewer at every phone/tablet/desktop width, in portrait, landscape and full screen. The REAL viewer is rendered
 * (SwiftShader WebGL) and the bounding rectangles of the elements as laid out by the browser are compared; a DOM node existing proves nothing here.
 */
const PORTRAIT = [
  { width: 320, height: 568 }, { width: 360, height: 800 }, { width: 390, height: 844 }, { width: 412, height: 915 },
  { width: 480, height: 854 }, { width: 768, height: 1024 }, { width: 1280, height: 800 },
];
const LANDSCAPE = [
  { width: 568, height: 320 }, { width: 800, height: 360 }, { width: 844, height: 390 }, { width: 915, height: 412 }, { width: 854, height: 480 },
];

const HINTS = '.scan-voxel-details, .scan-source-badge, .scan-mode-overlay, .scan-dimension-overlay, .scan-mesh-overlay, .axis-note, .scan-source-candidate-depth, .scan-source-highlight-status, .scan-source-history, .scan-sensor-details, .scan-overlay-failure';

interface Box { name: string; x: number; y: number; w: number; h: number }

async function open(page: Page): Promise<void> {
  await page.goto('/stress/scan-result-ui.html');
  await expect(page.locator('#fixture-state')).toHaveAttribute('data-loaded', 'true');
  await rendered(page);
  await page.locator('#mode-mesh').click();
  await rendered(page);
  await expect.poll(async () => page.locator('.scan-mesh-overlay').getAttribute('data-mesh-quality'), { timeout: 20_000 }).not.toBe('pending');
}

async function boxes(shell: Locator): Promise<{ shell: Box; controls: Box[]; hints: Box[] }> {
  return shell.evaluate((element, hintSelector) => {
    const take = (node: Element, name: string): Box => { const r = node.getBoundingClientRect(); return { name, x: r.x, y: r.y, w: r.width, h: r.height }; };
    const visible = (node: Element): boolean => {
      const style = getComputedStyle(node);
      const r = node.getBoundingClientRect();
      return style.display !== 'none' && style.visibility !== 'hidden' && r.width > 0 && r.height > 0;
    };
    const controls = [...element.querySelectorAll('.scan-camera-hud [data-camera-action]')].filter(visible).map((n) => take(n, `control:${n.getAttribute('data-camera-action')}`));
    const hints = [...element.querySelectorAll(hintSelector)].filter((n) => !n.closest('.scan-sensor-inset')).filter(visible).map((n) => take(n, `hint:${(n as HTMLElement).className.toString().split(' ')[0]}`));
    return { shell: take(element, 'shell'), controls, hints };
  }, HINTS);
}

const overlap = (a: Box, b: Box): number => Math.max(0, Math.min(a.x + a.w, b.x + b.w) - Math.max(a.x, b.x)) * Math.max(0, Math.min(a.y + a.h, b.y + b.h) - Math.max(a.y, b.y));

async function assertLayout(page: Page, label: string): Promise<void> {
  const shell = page.locator('.workspace .scan-viewport').first();
  await shell.scrollIntoViewIfNeeded();
  const { shell: frame, controls, hints } = await boxes(shell);
  expect(controls.length, `${label}: the camera strip has its buttons`).toBeGreaterThanOrEqual(6);
  const everything = [...controls, ...hints];
  const outside = everything.filter((b) => b.x < frame.x - 1 || b.x + b.w > frame.x + frame.w + 1 || b.y < frame.y - 1 || b.y + b.h > frame.y + frame.h + 1);
  expect(outside.map((b) => b.name), `${label}: everything stays inside the viewer`).toEqual([]);
  const collisions: string[] = [];
  for (let i = 0; i < everything.length; i += 1) for (let j = i + 1; j < everything.length; j += 1) {
    const area = overlap(everything[i], everything[j]);
    if (area > 4) collisions.push(`${everything[i].name} x ${everything[j].name} (${Math.round(area)} px2)`);
  }
  expect(collisions, `${label}: no control or hint covers another`).toEqual([]);
  for (const control of controls) expect(control.h, `${label}: ${control.name} is tappable`).toBeGreaterThanOrEqual(28);
  const scrollWidth = await page.evaluate(() => ({ scroll: document.documentElement.scrollWidth, inner: window.innerWidth }));
  expect(scrollWidth.scroll, `${label}: the page does not scroll sideways`).toBeLessThanOrEqual(scrollWidth.inner + 1);
  await test.info().attach(`layout ${label}`, { contentType: 'image/png', body: await shell.screenshot() });
}

test.beforeEach(async ({ page, api }) => {
  api.overrides.set('GET /stress/scan-result-ui.html', (route) => route.continue());
  await page.addInitScript(installGraphicsProbe);
});

for (const locale of ['en', 'uk'] as const) test.describe(`viewer layout ${locale}`, () => {
  test.use({ appLocale: locale });

  for (const viewport of PORTRAIT) {
    test(`LAYOUT portrait ${viewport.width}x${viewport.height}: controls and hints do not overlap`, async ({ page }) => {
      await page.setViewportSize(viewport);
      await open(page);
      await assertLayout(page, `${locale} ${viewport.width}x${viewport.height}`);
    });
  }

  for (const viewport of LANDSCAPE) {
    test(`LAYOUT landscape ${viewport.width}x${viewport.height}: controls and hints do not overlap`, async ({ page }) => {
      await page.setViewportSize(viewport);
      await open(page);
      await assertLayout(page, `${locale} ${viewport.width}x${viewport.height}`);
    });
  }

  for (const choice of ['esp32-preview', 'esp32-solver'] as const) for (const viewport of [{ width: 320, height: 568 }, { width: 390, height: 844 }, { width: 844, height: 390 }]) {
    test(`LAYOUT ${choice} voxels ${viewport.width}x${viewport.height}: the voxel metadata does not cover the controls`, async ({ page }) => {
      await page.setViewportSize(viewport);
      await open(page);
      await page.locator('.scan-mode-select').selectOption(choice);
      await expect(page.locator('.scan-voxel-details'), 'the metadata of the chosen representation is shown').toBeVisible({ timeout: 20_000 });
      await assertLayout(page, `${locale} ${choice} ${viewport.width}x${viewport.height}`);
    });
  }

  for (const viewport of [{ width: 390, height: 844 }, { width: 844, height: 390 }]) {
    test(`LAYOUT fullscreen at ${viewport.width}x${viewport.height}: controls and hints do not overlap and Exit full screen is reachable`, async ({ page }) => {
      await page.setViewportSize(viewport);
      await open(page);
      const toggle = page.locator('.scan-camera-hud [data-camera-action="fullscreen"]');
      await expect(toggle, 'this browser offers full screen').toBeVisible();
      await toggle.click();
      await expect(toggle, 'the viewer is in full screen').toHaveAttribute('aria-pressed', 'true');
      await assertLayout(page, `${locale} fullscreen ${viewport.width}x${viewport.height}`);
      await toggle.click();
      await expect(toggle).toHaveAttribute('aria-pressed', 'false');
    });
  }
});
