import { describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { create, act, type ReactTestRenderer } from 'react-test-renderer';
import { renderToStaticMarkup } from 'react-dom/server';

import { ScanRepresentationSelect, type Esp32PreviewAvailability } from '../../src/components/ScanRepresentationSelect';
import { LocaleProvider, translate } from '../../src/i18n';

const markup = (props: Partial<Parameters<typeof ScanRepresentationSelect>[0]>, locale: 'en' | 'uk' = 'en'): string =>
  renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: locale }, createElement(ScanRepresentationSelect, {
    representation: 'mesh', meshActive: false, preview: 'absent', onChoose: () => undefined, ...props,
  })));

describe('3D representation select inside the existing Mesh entry (issue #44)', () => {
  it.each(['en', 'uk'] as const)('is one native select with an accessible name, a neutral caption while another mode is active (%s)', (locale) => {
    const html = markup({}, locale);
    expect(html.startsWith('<select')).toBe(true);
    expect(html).toContain(`aria-label="${translate(locale, 'scan.representation.label')}"`);
    expect(html).toContain('data-active="false"');
    expect(html).toContain(translate(locale, 'scan.mode.mesh'));
  });

  it('offers the voxels only when there is something to show, and says so while they are being prepared', () => {
    const options = (preview: Esp32PreviewAvailability) => [...markup({ preview }).matchAll(/<option value="([^"]*)"/g)].map((match) => match[1]);
    expect(options('absent')).toEqual(['', 'mesh']);
    expect(options('preparing')).toEqual(['', 'mesh', 'esp32-preview']);
    expect(options('ready')).toEqual(['', 'mesh', 'esp32-preview']);
    expect(markup({ preview: 'preparing' })).toContain(translate('en', 'scan.representation.esp32_preview_preparing'));
    expect(markup({ preview: 'preparing' })).toMatch(/<option value="esp32-preview"[^>]*disabled/);
    expect(markup({ preview: 'ready' })).not.toMatch(/<option value="esp32-preview"[^>]*disabled/);
    expect(markup({ preview: 'ready' })).toContain(translate('en', 'scan.representation.esp32_preview'));
  });

  it('offers the solver voxels as a third choice, only when they exist, and keeps that choice readable', () => {
    const options = (solver: Esp32PreviewAvailability) => [...markup({ preview: 'ready', solver }).matchAll(/<option value="([^"]*)"/g)].map((match) => match[1]);
    expect(options('absent')).toEqual(['', 'mesh', 'esp32-preview']);
    expect(options('ready')).toEqual(['', 'mesh', 'esp32-preview', 'esp32-solver']);
    expect(markup({ preview: 'ready', solver: 'preparing' })).toMatch(/<option value="esp32-solver"[^>]*disabled/);
    expect(markup({ preview: 'ready', solver: 'preparing' })).toContain(translate('en', 'scan.representation.esp32_solver_preparing'));
    expect(markup({ representation: 'esp32-solver', preview: 'ready', solver: 'ready' })).toMatch(/<option value="esp32-solver"[^>]*selected/);
  });

  it('keeps the selection readable in the control: Mesh when the mesh mode is active, the voxels when chosen, neutral otherwise', () => {
    expect(markup({ meshActive: true, preview: 'ready' })).toMatch(/<option value="mesh"[^>]*selected/);
    expect(markup({ representation: 'esp32-preview', preview: 'ready' })).toMatch(/<option value="esp32-preview"[^>]*selected/);
    expect(markup({ representation: 'esp32-preview', meshActive: true, preview: 'ready' })).toMatch(/<option value="esp32-preview"[^>]*selected/);
    expect(markup({ meshActive: false, preview: 'ready' })).toMatch(/<option value=""[^>]*selected/);
  });

  it('reports the choice and ignores the neutral caption', async () => {
    const onChoose = vi.fn();
    let renderer!: ReactTestRenderer;
    await act(async () => {
      renderer = create(createElement(LocaleProvider, { initialLocale: 'en' }, createElement(ScanRepresentationSelect, {
        representation: 'mesh', meshActive: false, preview: 'ready', solver: 'ready', onChoose,
      })));
    });
    const select = renderer.root.findByType('select');
    act(() => { select.props.onChange({ target: { value: 'esp32-preview' } }); });
    act(() => { select.props.onChange({ target: { value: 'esp32-solver' } }); });
    act(() => { select.props.onChange({ target: { value: 'mesh' } }); });
    act(() => { select.props.onChange({ target: { value: '' } }); });
    expect(onChoose.mock.calls).toEqual([['esp32-preview'], ['esp32-solver'], ['mesh']]);
  });
});
