import { test, expect } from './fixtures/device';
import { assertLayout, assertReachable, capture, openSection } from './helpers/layout';
import { assertSemanticStyles } from './helpers/theme';

for (const fault of ['width', 'position', 'text', 'color', 'blocked'] as const) {
  test(`detector rejects ${fault} and restored page passes`, async ({ page }) => {
    await page.goto('/');
    await openSection(page, fault === 'position' ? 'scanner' : 'objects', 'en');
    const check = fault === 'color' ? assertSemanticStyles : fault === 'blocked' ? assertReachable : assertLayout;
    await check(page);
    const css = {
      width: '.console-card { min-width: 2000px !important; }',
      position: '.console-floating-back { position:fixed !important;top:1rem !important;left:1rem !important;z-index:9999 !important; }',
      text: '.object-workspace h1 { height:8px !important;overflow:hidden !important;line-height:1 !important; }',
      color: '.console-actions button.danger { background:rgb(255,0,255) !important; }',
      blocked: '.device-console-shell::after { content:"";position:fixed;inset:0;z-index:99999; }',
    }[fault];
    const style = await page.addStyleTag({ content: css });
    let caught = '';
    try { await check(page); } catch (error) { caught = String(error); }
    await capture(page, `injected-${fault}`);
    await test.info().attach('detected-fault', { body: caught, contentType: 'text/plain' });
    expect(caught, 'the unchanged checker must reject the injected defect').not.toBe('');
    await style.evaluate((node) => node.parentNode?.removeChild(node));
    await page.evaluate(() => window.scrollTo(0, 0));
    await check(page);
    await capture(page, `restored-${fault}`);
  });
}
