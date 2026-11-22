import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

/*
 * After a scan the page is a flex column whose order is set by CSS. Phone run 2026-11-22: the scanner card (Continue scan / Scan again) was ordered LAST,
 * under the whole viewer, while "Continue to recording" sits at the top. The main buttons belong above the viewer.
 */
const css = readFileSync(resolve(__dirname, '../../src/scan.css'), 'utf-8');
const orderOf = (selector: string): number => {
  const start = css.indexOf(`${selector} {`);
  expect(start, `rule for ${selector}`).toBeGreaterThanOrEqual(0);
  const body = css.slice(start, css.indexOf('}', start));
  const order = /order:\s*(-?\d+)/.exec(body);
  expect(order, `order in ${selector}`).not.toBeNull();
  return Number(order![1]);
};

describe('the page after a scan', () => {
  it('puts the scanner card with Continue scan above the mode bar and the viewer', () => {
    const card = orderOf('.app-shell--result > .xr-runtime-card');
    expect(card).toBeLessThan(orderOf('.app-shell--result > .scan-toolbar'));
    expect(card).toBeLessThan(orderOf('.app-shell--result > .workspace'));
  });
});
