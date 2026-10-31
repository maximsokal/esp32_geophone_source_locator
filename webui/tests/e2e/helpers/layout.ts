import { expect, test, type Page } from '@playwright/test';
import contract from '../../../../tests/ui_contract.json' with { type: 'json' };
import { catalog } from '../fixtures/device';

export { contract };
export const sections = ['home', 'objects', 'sensors', 'workflow', 'scanner'] as const;
export type Section = typeof sections[number];
export type Locale = 'en' | 'uk';

export async function openSection(page: Page, section: Section, locale: Locale) {
  const t = catalog[locale];
  if (await page.locator('.scanner-feature-shell').count()) {
    await page.getByRole('button', { name: t['console.back_object'], exact: true }).first().click();
  }
  const names = { home: t['nav.home'], objects: t['nav.objects'], sensors: t['nav.sensors'],
    workflow: t['workflow.nav'], scanner: t['workflow.nav'] };
  await page.getByRole('navigation').getByRole('button', { name: names[section], exact: true }).click();
  if (section === 'scanner') {
    await page.getByRole('button', { name: t['workflow.action.scan_start'], exact: true }).click();
    await expect(page.getByRole('heading', { name: t['scan.search_volume_title'], exact: true })).toBeVisible();
  } else if (section === 'sensors') {
    await expect(page.getByRole('cell', { name: t['nrf_log.panel.no_records'], exact: true })).toBeVisible();
  }
  await page.evaluate(() => document.fonts.ready);
  await page.evaluate(() => window.scrollTo(0, 0));
}

export async function capture(page: Page, name: string) {
  const path = test.info().outputPath(`${name}.png`);
  await page.screenshot({ path, fullPage: true, animations: 'disabled' });
  await test.info().attach(name, { path, contentType: 'image/png' });
}

export async function layoutIssues(page: Page): Promise<string[]> {
  return page.evaluate(({ roundingPx: eps, controlMinPx, sectionGapPx, actionGapPx }) => {
    const issues: string[] = [];
    const width = document.documentElement.clientWidth;
    const visible = (el: Element) => {
      const s = getComputedStyle(el);
      return s.visibility !== 'hidden' && el.getClientRects().length > 0 && !el.closest('.sr-only');
    };
    const label = (el: Element) => `${el.tagName.toLowerCase()}.${el.className.toString().replaceAll(' ', '.')} ${(el.textContent ?? '').trim().slice(0, 65)}`;
    const rect = (el: Element) => el.getBoundingClientRect();
    const intersects = (a: DOMRect, b: DOMRect) => Math.min(a.right, b.right) - Math.max(a.left, b.left) > eps &&
      Math.min(a.bottom, b.bottom) - Math.max(a.top, b.top) > eps;
    if (document.documentElement.scrollWidth > width + eps) {
      issues.push(`page overflow: actual=${document.documentElement.scrollWidth}px expected<=${width}px`);
    }
    for (const el of document.querySelectorAll('button,select,input:not([type=radio]):not([type=checkbox]),h1,h2,h3,p,dt,dd,.console-card,.section-heading,.console-page,.app-header,.header-badges')) {
      if (!visible(el)) continue;
      // This one production table has an explicit local horizontal scroll container.
      if (el.closest('.nrf-log-table-wrap')) continue;
      const r = rect(el);
      if (r.left < -eps || r.right > width + eps) issues.push(`bounds ${label(el)}: actual=[${r.left},${r.right}] expected=[0,${width}]`);
      if (el.matches('button,select,input')) {
        if (r.height < controlMinPx - eps || r.width < controlMinPx - eps) {
          issues.push(`target ${label(el)}: actual=${r.width}x${r.height} expected>=${controlMinPx}x${controlMinPx}`);
        }
      }
      if (el.matches('button,h1,h2,h3,p,dt,dd') && el.scrollWidth > el.clientWidth + eps) {
        issues.push(`text overflow ${label(el)}: actual=${el.scrollWidth}px expected<=${el.clientWidth}px`);
      }
      for (const node of el.childNodes) {
        if (node.nodeType !== Node.TEXT_NODE || !node.textContent?.trim()) continue;
        const range = document.createRange(); range.selectNodeContents(node);
        const style = getComputedStyle(el);
        const clipsX = ['hidden', 'clip'].includes(style.overflowX);
        const clipsY = ['hidden', 'clip'].includes(style.overflowY);
        for (const line of range.getClientRects()) {
          // Font glyph boxes can extend a visible line box; only actual clipping is forbidden.
          if ((clipsX && (line.left < r.left - eps || line.right > r.right + eps)) ||
              (clipsY && (line.top < r.top - eps || line.bottom > r.bottom + eps))) {
            issues.push(`clipped text ${label(el)}: text=[${line.left},${line.top},${line.right},${line.bottom}] box=[${r.left},${r.top},${r.right},${r.bottom}]`);
          }
        }
      }
    }
    const sensorPage = document.querySelector('.sensors-page');
    // Match the old page too, so a missing class cannot turn the regression check off.
    const sensorRoot = sensorPage ?? document.querySelector('.pod-grid')?.parentElement;
    if (sensorRoot) {
      const blocks = [...sensorRoot.children].filter(visible);
      for (let i = 1; i < blocks.length; i++) {
        const gap = rect(blocks[i]).top - rect(blocks[i - 1]).bottom;
        if (gap < sectionGapPx - eps) issues.push(`section gap ${label(blocks[i])}: actual=${gap}px expected>=${sectionGapPx}px`);
      }
      const calibrate = sensorRoot.querySelector('.card-heading button');
      const pill = calibrate?.parentElement?.querySelector('.state-pill');
      if (calibrate && pill) {
        const a = rect(pill), b = rect(calibrate);
        const gap = Math.max(b.left - a.right, a.left - b.right, b.top - a.bottom, a.top - b.bottom);
        if (gap < actionGapPx - eps) issues.push(`leveling action gap: actual=${gap}px expected>=${actionGapPx}px`);
      }
    }
    const back = document.querySelector('.console-floating-back');
    const heading = document.querySelector('.scanner-feature-shell h1');
    if (back && heading && intersects(rect(back), rect(heading))) issues.push('scanner Back overlaps heading');
    for (const container of document.querySelectorAll('.console-page,.object-workspace,.dashboard-grid,.card-heading,.section-heading,.console-actions,.card-actions,.pod-grid')) {
      const children = [...container.children].filter(visible);
      for (let i = 0; i < children.length; i++) for (let j = i + 1; j < children.length; j++) {
        const a = rect(children[i]), b = rect(children[j]);
        if (intersects(a, b)) issues.push(`sibling overlap: ${label(children[i])} / ${label(children[j])}`);
        const minGap = container.matches('.console-actions,.card-actions') ? actionGapPx :
          container.matches('.dashboard-grid,.object-workspace,.pod-grid') ? sectionGapPx : 0;
        const gap = Math.max(b.left - a.right, a.left - b.right, b.top - a.bottom, a.top - b.bottom);
        if (minGap && gap < minGap - eps) issues.push(`shared gap ${label(container)}: actual=${gap}px expected>=${minGap}px`);
      }
    }
    // A stacked (grid) heading must start every child's text at the container's left edge; a leftover
    // flex `justify-content` once centred the single grid column and indented the title.
    for (const heading of document.querySelectorAll('.card-heading,.section-heading')) {
      if (getComputedStyle(heading).display !== 'grid') continue;
      const edge = rect(heading).left;
      for (const child of [...heading.children].filter(visible)) {
        // Text blocks are judged by where their text starts; boxed controls (button, pill) by their own edge.
        const range = document.createRange(); range.selectNodeContents(child);
        const left = child.matches('h1,h2,h3,h4,p,div') ? range.getBoundingClientRect().left : rect(child).left;
        if (left > edge + eps) issues.push(`indented heading ${label(child)}: actual=${left}px expected=${edge}px`);
      }
    }
    for (const wrapper of document.querySelectorAll<HTMLElement>('.nrf-log-table-wrap')) {
      const saved = wrapper.scrollLeft;
      wrapper.scrollLeft = wrapper.scrollWidth;
      const last = wrapper.querySelector('th:last-child');
      if (last && rect(last).right > rect(wrapper).right + eps) issues.push('last nRF log column cannot be exposed by local scrolling');
      wrapper.scrollLeft = saved;
    }
    return issues;
  }, contract.geometry);
}

export async function assertLayout(page: Page) {
  const issues = await layoutIssues(page);
  await test.info().attach('geometry', { body: JSON.stringify({ viewport: page.viewportSize(), issues }, null, 2), contentType: 'application/json' });
  expect(issues, 'DOM bounds, text, action sizes and shared spacing').toEqual([]);
}

export async function assertReachable(page: Page) {
  const buttons = page.locator('button:visible:enabled,select:visible:enabled,summary:visible,[tabindex="0"]:visible');
  for (const el of await buttons.all()) {
    await el.evaluate((node) => node.scrollIntoView({ block: 'center', inline: 'nearest' }));
    const hit = await el.evaluate((node) => {
      const r = node.getBoundingClientRect();
      return node.contains(document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2));
    });
    expect(hit, `action is covered: ${await el.textContent()}`).toBe(true);
  }
}
