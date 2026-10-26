import { createHash } from 'node:crypto';
import { existsSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { expect, test } from '@playwright/test';

/*
 * Browser checks of the TFT sensor-check screen. The screen is rendered by the
 * real LVGL on a native host into .test-build/ui-render/latest (see
 * tools/run_ui_render_tests.py); Chromium opens that gallery. The physical
 * display is not driven here.
 */
const repository = resolve(process.cwd(), '..');
const gallery = resolve(repository, '.test-build/ui-render/latest');
const goldens = resolve(repository, 'tests/ui_render/golden');
const index = pathToFileURL(resolve(gallery, 'index.html')).href;

type Widget = { role: string; hidden: boolean; text?: string; bounds: [number, number, number, number] };
type Frame = { name: string; golden: boolean; comparison: number; step: number; widgets: Widget[] };

const sha = (path: string) => createHash('sha256').update(readFileSync(path)).digest('hex');

function loadFrames(): Map<string, Frame> {
  const lines = readFileSync(resolve(gallery, 'frames.jsonl'), 'utf8').split('\n').filter(Boolean);
  return new Map(lines.map((line) => {
    const frame = JSON.parse(line) as Frame;
    return [frame.name, frame];
  }));
}

function texts(frame: Frame): Record<string, string> {
  return Object.fromEntries(frame.widgets.filter((w) => !w.hidden && w.text).map((w) => [w.role, w.text as string]));
}

const languages = {
  en: { legend: /^hPa \/ °C \/ gyro °\/s \| MAIN /, status: 'N+ I+ B+ C-' },
  uk: { legend: /^гПа \/ °C \/ гіро °\/с \| MAIN /, status: 'N+ I+ B+ C-' },
} as const;

test.beforeAll(() => {
  expect(existsSync(resolve(gallery, 'index.html')),
    'render the gallery first: python tools/run_ui_render_tests.py (npm run test:ui:tft does it)').toBe(true);
});

for (const language of ['en', 'uk'] as const) {
  test.describe(`TFT sensor values, ${language}`, () => {
    test('values page shows pressure, temperature and gyroscope on the node cards', async () => {
      const all = loadFrames();
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

    test('every value fits its card and the screen', async () => {
      for (const name of [`${language}_sensor_values`, `${language}_sensor_values_partial`]) {
        const frame = loadFrames().get(name) as Frame;
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
      await page.goto(index);
      await page.selectOption('#family', 'workflow');
      await page.selectOption('#language', language);
      for (const name of [`${language}_sensor_values`, `${language}_sensor_values_partial`]) {
        const card = page.locator(`figure[data-lang="${language}"]:has(img[alt="${name}"])`);
        await expect(card, name).toBeVisible();
        await expect(card, name).toHaveAttribute('data-result', '1'); /* matches the reference */
        const image = card.locator('img');
        await expect(image).toHaveJSProperty('naturalWidth', 320);
        await expect(image).toHaveJSProperty('naturalHeight', 240);
        expect(sha(resolve(gallery, `${name}.png`)), `${name} differs from the reference PNG`)
          .toBe(sha(resolve(goldens, `${name}.png`)));
        await test.info().attach(name, { body: await image.screenshot(), contentType: 'image/png' });
      }
      /* The language filter hides the other locale's frames. */
      const other = language === 'en' ? 'uk' : 'en';
      await expect(page.locator(`figure[data-lang="${other}"]`).first()).toBeHidden();
    });

    test('the values page is visibly different from the status page of the same screen', async ({ page }) => {
      const statusName = `${language}_preflight_uncalibrated`;
      const status = loadFrames().get(statusName) as Frame;
      expect(texts(status).remote_text_0).toContain(languages[language].status);

      const shots: Record<string, Buffer> = {};
      for (const name of [statusName, `${language}_sensor_values`]) {
        await page.goto(pathToFileURL(resolve(gallery, `${name}.png`)).href);
        shots[name] = await page.locator('img').screenshot();
      }
      expect(Buffer.compare(shots[statusName], shots[`${language}_sensor_values`])).not.toBe(0);
    });
  });
}

test('both locales show the same values on the same data', async () => {
  const all = loadFrames();
  for (const suffix of ['sensor_values', 'sensor_values_partial']) {
    const english = texts(all.get(`en_${suffix}`) as Frame);
    const ukrainian = texts(all.get(`uk_${suffix}`) as Frame);
    for (const role of ['remote_text_0', 'remote_text_1', 'remote_text_2', 'remote_text_3']) {
      expect(ukrainian[role], `${suffix} ${role}`).toBe(english[role]);
    }
  }
});
