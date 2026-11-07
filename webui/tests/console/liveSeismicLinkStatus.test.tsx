import { beforeAll, describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import type { DeviceNodeLink } from '../../src/domain/device';
import LiveSeismicPanel from '../../src/features/seismic/LiveSeismicPanel';
import { nodeLinkClass, waveformLinkStatus } from '../../src/features/sensors/nodeLinkStatus';
import { loadUkrainianCatalog, LocaleProvider, translate, type I18nLocale } from '../../src/i18n';

const links: DeviceNodeLink[] = [
  { channel: 4, node: 'G3', status: 'MEASURING' },
  { channel: 1, node: 'MAIN', status: 'NOT_CONNECTED' },
  { channel: 3, node: 'G2', status: 'READY' },
  { channel: 2, node: 'G1', status: 'CONNECTED' },
];

function live(locale: I18nLocale, nodeLinks: DeviceNodeLink[] | undefined, statusStale = false): string {
  return renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: locale },
    createElement(LiveSeismicPanel, { nodeLinks, statusStale })));
}

/* The class of the label of one graph row, e.g. "G2" -> "seismic-link seismic-link--ready". */
function labelClass(html: string, label: string): string | undefined {
  return html.match(new RegExp(`<span class="(seismic-link[^"]*)"[^>]*>${label}`))?.[1];
}

beforeAll(async () => { await loadUkrainianCatalog(); });

describe('Live graph link status (TFT parity: API channel n -> waveform n-1)', () => {
  it('maps 1-based channel to 0-based waveform regardless of array order', () => {
    expect([0, 1, 2, 3].map((i) => waveformLinkStatus(links, i)))
      .toEqual(['NOT_CONNECTED', 'CONNECTED', 'READY', 'MEASURING']);
    expect([0, 1, 2, 3].map((i) => waveformLinkStatus([...links].reverse(), i)))
      .toEqual(['NOT_CONNECTED', 'CONNECTED', 'READY', 'MEASURING']);
  });

  it('does not guess for missing, duplicated or out-of-range channels', () => {
    const odd: DeviceNodeLink[] = [
      { channel: 0, node: 'X', status: 'READY' },
      { channel: 2, node: 'G1', status: 'READY' },
      { channel: 2, node: 'G1b', status: 'MEASURING' },
      { channel: 9, node: 'Y', status: 'READY' },
    ];
    expect([0, 1, 2, 3].map((i) => waveformLinkStatus(odd, i))).toEqual([null, null, null, null]);
    expect(waveformLinkStatus(undefined, 0)).toBeNull();
  });

  it('keeps stale or unknown statuses out of green/blue', () => {
    expect(nodeLinkClass('READY', false)).toBe('ready');
    expect(nodeLinkClass('READY', true)).toBe('unknown');
    expect(nodeLinkClass('MEASURING', true)).toBe('unknown');
    expect(nodeLinkClass('NOT_CONNECTED', true)).toBe('not-connected');
    expect(nodeLinkClass('BOGUS', false)).toBe('unknown');
  });

  it.each(['en', 'uk'] as const)('colours and names every graph label by its link in %s', (locale) => {
    const html = live(locale, links);
    expect(labelClass(html, 'G1')).toBe('seismic-link seismic-link--not-connected');
    expect(labelClass(html, 'G2')).toBe('seismic-link seismic-link--connected');
    expect(labelClass(html, 'G3')).toBe('seismic-link seismic-link--ready');
    expect(labelClass(html, 'G4')).toBe('seismic-link seismic-link--measuring');
    expect(html).toContain(translate(locale, 'sensors.node_link.measuring'));
  });

  it('shows no link colour without status, and neutral READY when stale', () => {
    const none = live('en', undefined);
    for (const g of ['G1', 'G2', 'G3', 'G4']) expect(labelClass(none, g)).toBe('seismic-link seismic-link--unknown');
    const stale = live('en', links, true);
    expect(labelClass(stale, 'G1')).toBe('seismic-link seismic-link--not-connected');
    expect(labelClass(stale, 'G3')).toBe('seismic-link seismic-link--unknown');
    expect(labelClass(stale, 'G4')).toBe('seismic-link seismic-link--unknown');
  });
});
