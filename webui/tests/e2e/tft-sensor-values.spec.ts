import { expect, test } from '@playwright/test';
import type { Page } from '@playwright/test';

/*
 * Browser checks of the TFT sensor-check screen. The screen is rendered by the
 * real LVGL on a native host into .test-build/ui-render/latest (see
 * tools/run_ui_render_tests.py); Chromium opens that gallery. The physical
 * display is not driven here.
 *
 * No Node API is used: the project carries no Node type definitions. The rendered
 * gallery and the accepted references are handed to the browser through
 * page.route() (route.fulfill() reads the files), under fixed https origins so that
 * fetch() and crypto.subtle work, and everything else is read inside the page.
 */
const GALLERY = 'https://gallery.test';
const GOLDENS = 'https://goldens.test';
const MISSING = 'render the gallery first: python tools/run_ui_render_tests.py (npm run test:ui:tft does it)';

type Widget = { role: string; hidden: boolean; text?: string; bounds: [number, number, number, number] };
type Frame = { name: string; golden: boolean; comparison: number; step: number; widgets: Widget[] };

async function openGallery(page: Page): Promise<void> {
  /* route.fulfill() resolves a relative path against the working directory, which is webui. */
  const folders: Record<string, string> = {
    'gallery.test': '../.test-build/ui-render/latest',
    'goldens.test': '../tests/ui_render/golden',
  };
  await page.route(/^https:\/\/(gallery|goldens)\.test\//, async (route) => {
    const url = new URL(route.request().url());
    const name = decodeURIComponent(url.pathname.slice(1));
    if (name === '' || name.split('/').includes('..') || name.includes('\\')) {
      await route.fulfill({ status: 404 });
      return;
    }
    try {
      await route.fulfill({ path: `${folders[url.hostname]}/${name}`, headers: { 'access-control-allow-origin': '*' } });
    } catch {
      await route.fulfill({ status: 404, body: MISSING });
    }
  });
  const response = await page.goto(`${GALLERY}/index.html`);
  expect(response?.ok(), MISSING).toBe(true);
}

async function loadFrames(page: Page, names: string[]): Promise<Map<string, Frame>> {
  const frames = await page.evaluate(async ({ url, wanted }) => {
    const text = await (await fetch(url)).text();
    const found: Frame[] = [];
    for (const line of text.split('\n')) {
      if (line === '') continue;
      const frame = JSON.parse(line) as Frame;
      if (wanted.includes(frame.name)) found.push(frame);
    }
    return found;
  }, { url: `${GALLERY}/frames.jsonl`, wanted: names });
  const byName = new Map(frames.map((frame): [string, Frame] => [frame.name, frame]));
  for (const name of names) expect(byName.has(name), `${name} is missing from frames.jsonl`).toBe(true);
  return byName;
}

async function sha256(page: Page, url: string): Promise<string> {
  return page.evaluate(async (target) => {
    const response = await fetch(target);
    if (!response.ok) throw new Error(`${target}: HTTP ${response.status}`);
    const hash = await crypto.subtle.digest('SHA-256', await response.arrayBuffer());
    return Array.from(new Uint8Array(hash), (byte) => byte.toString(16).padStart(2, '0')).join('');
  }, url);
}

function texts(frame: Frame): Record<string, string> {
  return Object.fromEntries(frame.widgets.filter((w) => !w.hidden && w.text).map((w) => [w.role, w.text as string]));
}

const languages = {
  en: { legend: /^hPa \/ °C \/ gyro °\/s \| MAIN /, status: 'N+ I+ B+ C-' },
  uk: { legend: /^гПа \/ °C \/ гіро °\/с \| MAIN /, status: 'N+ I+ B+ C-' },
} as const;

for (const language of ['en', 'uk'] as const) {
  test.describe(`TFT sensor values, ${language}`, () => {
    test('values page shows pressure, temperature and gyroscope on the node cards', async ({ page }) => {
      await openGallery(page);
      const all = await loadFrames(page, [`${language}_sensor_values`, `${language}_sensor_values_partial`]);
      const single = texts(all.get(`${language}_sensor_values`) as Frame);
      expect(single.remote_text_0).toBe('G1 1010.1 24.1 0.3');
      expect(single.remote_text_1).toBe('G2 -- -- --');
      expect(single.footer).toMatch(languages[language].legend);
      expect(single.footer).toContain('MAIN 1010.1 24.1');

      const partial = texts(all.get(`${language}_sensor_values_partial`) as Frame);
      expect(partial.remote_text_0).toBe('G1 1000.0 -5.5 12.0');
      expect(partial.remote_text_1).toBe('G2 999.5 21.0 --');
      expect(partial.remote_text_2).toBe('G3 -- -- --');
      expect(partial.remote_text_3).toBe('G4 -- -- --');
      for (const role of ['remote_text_0', 'remote_text_1', 'remote_text_2', 'remote_text_3']) {
        expect(partial[role], role).not.toMatch(/NaN|undefined|null|-0\.0/);
      }
    });

    test('every value fits its card and the screen', async ({ page }) => {
      await openGallery(page);
      const names = [`${language}_sensor_values`, `${language}_sensor_values_partial`];
      const all = await loadFrames(page, names);
      for (const name of names) {
        const frame = all.get(name) as Frame;
        for (const widget of frame.widgets.filter((w) => !w.hidden && w.text)) {
          const [x, y, right, bottom] = widget.bounds;
          expect(x, `${name} ${widget.role}`).toBeGreaterThanOrEqual(0);
          expect(y, `${name} ${widget.role}`).toBeGreaterThanOrEqual(0);
          expect(right, `${name} ${widget.role}`).toBeLessThan(320);
          expect(bottom, `${name} ${widget.role}`).toBeLessThan(240);
          if (widget.role.startsWith('remote_text_')) {
            /* A card label is 130 px wide; the values line must not spill into the next card. */
            expect(right - x, `${name} ${widget.role}`).toBeLessThanOrEqual(131);
          }
        }
      }
    });

    test('the browser gallery serves the accepted 320x240 frames', async ({ page }) => {
      await openGallery(page);
      await page.selectOption('#family', 'workflow');
      await page.selectOption('#language', language);
      for (const name of [`${language}_sensor_values`, `${language}_sensor_values_partial`]) {
        const card = page.locator(`figure[data-lang="${language}"]:has(img[alt="${name}"])`);
        await expect(card, name).toBeVisible();
        await expect(card, name).toHaveAttribute('data-result', '1'); /* matches the reference */
        const image = card.locator('img');
        await expect(image).toHaveJSProperty('naturalWidth', 320);
        await expect(image).toHaveJSProperty('naturalHeight', 240);
        expect(await sha256(page, `${GALLERY}/${name}.png`), `${name} differs from the reference PNG`)
          .toBe(await sha256(page, `${GOLDENS}/${name}.png`));
        await test.info().attach(name, { body: await image.screenshot(), contentType: 'image/png' });
      }
      /* The language filter hides the other locale's frames. */
      const other = language === 'en' ? 'uk' : 'en';
      await expect(page.locator(`figure[data-lang="${other}"]`).first()).toBeHidden();
    });

    test('the values page is visibly different from the status page of the same screen', async ({ page }) => {
      await openGallery(page);
      const statusName = `${language}_preflight_uncalibrated`;
      const valuesName = `${language}_sensor_values`;
      const all = await loadFrames(page, [statusName]);
      expect(texts(all.get(statusName) as Frame).remote_text_0).toContain(languages[language].status);
      expect(await sha256(page, `${GALLERY}/${statusName}.png`)).not.toBe(await sha256(page, `${GALLERY}/${valuesName}.png`));
    });
  });
}

test('both locales show the same values on the same data', async ({ page }) => {
  await openGallery(page);
  const suffixes = ['sensor_values', 'sensor_values_partial'];
  const all = await loadFrames(page, suffixes.flatMap((suffix) => [`en_${suffix}`, `uk_${suffix}`]));
  for (const suffix of suffixes) {
    const english = texts(all.get(`en_${suffix}`) as Frame);
    const ukrainian = texts(all.get(`uk_${suffix}`) as Frame);
    for (const role of ['remote_text_0', 'remote_text_1', 'remote_text_2', 'remote_text_3']) {
      expect(ukrainian[role], `${suffix} ${role}`).toBe(english[role]);
    }
  }
});
