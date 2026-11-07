import { describe, expect, it } from 'vitest';
import { renderReportDocument } from '../../src/features/report/reportExport';
import { uiTheme } from '../../src/theme';

/* S12 (REGRESSION): the printed report stays a white document with dark text,
 * independent of the dark instrument theme of the console. */
describe('printed report keeps a light document theme', () => {
  const doc = renderReportDocument('<p>body</p>', 'Report', 'en');

  it('carries none of the dark instrument theme colours or theme variables', () => {
    for (const colour of Object.values(uiTheme)) expect(doc.toLowerCase()).not.toContain(colour.toLowerCase());
    expect(doc).not.toContain('--ui-');
  });

  it('never paints the page or tables with a background colour', () => {
    const bodyRule = doc.match(/\bbody\{[^}]*\}/)?.[0] ?? '';
    expect(bodyRule).not.toBe('');
    expect(bodyRule).not.toMatch(/background/);
    expect(bodyRule).toMatch(/color:#111/);
  });
});
