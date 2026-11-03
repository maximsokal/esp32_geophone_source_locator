import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { beforeAll, describe, expect, it } from 'vitest';

import { ScanDistanceRefinementCard } from '../../src/components/ScanDistanceRefinementCard';
import { loadUkrainianCatalog, translate, type I18nLocale } from '../../src/i18n';
import type { LocalizationSessionGeometry } from '../../src/api/deviceApi';
import type { RegistrationArtifact } from '../../src/xr/fiducial/registrationArtifact';
import { computeScanDistanceRefinement } from '../../src/xr/scanDistanceRefinement';

beforeAll(loadUkrainianCatalog);

const GEOMETRY: LocalizationSessionGeometry = {
  available: true,
  pods: [
    { pod: 0, node_id: 0, computed: true, assigned: true, leveled: true },
    { pod: 1, node_id: 1, computed: true, assigned: true, leveled: true, x_mm: 3000, y_mm: 0, z_mm: 0 },
    { pod: 2, node_id: 2, computed: true, assigned: true, leveled: true, x_mm: 0, y_mm: 4000, z_mm: 0 },
  ],
};

function artifact(bindings: RegistrationArtifact['bindings']): RegistrationArtifact {
  return {
    schema: 1, frame: 'webxr_local_floor', scan_frame_id: 'scan-crc32:00000001', source: 'apriltag', family: 'tag36h11',
    geometry_computed_at_ms: 1, created_at_ms: 2, tag_size_m: { '0': 0.032, '1': 0.032, '2': 0.032, '3': 0.032, '4': 0.032 },
    bindings, sensor_to_scan: null, mirrored: false, residual_rms_m: 0, threshold_m: 0.05, per_node: [], tier: null, valid: false, reason: null,
  };
}

const tag = (node: 'MAIN' | 'G1' | 'G2', tagId: number, position: [number, number, number]) =>
  ({ tag_id: tagId, node, observation_count: 9, inlier_count: 8, position_scan_m: position, spread_m: 0.01 });

function render(locale: I18nLocale, registration: RegistrationArtifact | null): string {
  return renderToStaticMarkup(createElement(ScanDistanceRefinementCard, { locale, refinement: computeScanDistanceRefinement(GEOMETRY, registration) }));
}

describe('scan information: distances recalculated from the scan', () => {
  const scanned = artifact([tag('MAIN', 0, [0, 0, 0]), tag('G1', 1, [2.8, 0, 0]), tag('G2', 2, [0, 4.1, 0])]);

  it.each(['en', 'uk'] as const)('lists the devices, every recalculated pair and the error reduction in %s', (locale) => {
    const html = render(locale, scanned);
    expect(html).toContain(translate(locale, 'scan.refine.title'));
    expect(html).toContain('MAIN, G1, G2');
    for (const pair of ['MAIN-G1', 'MAIN-G2', 'G1-G2']) expect(html).toContain(`data-pair="${pair}"`);
    /* MAIN-G1: CS 3.00 m, scan 2.80 m, correction -200 mm; the numbers are in the pair line. */
    expect(html).toMatch(/data-pair="MAIN-G1">[^<]*2[.,]80/);
    expect(html).toContain('-200');
    const percent = /data-reduction-percent="([0-9.]+)"/.exec(html);
    expect(percent).not.toBeNull();
    expect(Number(percent![1])).toBeGreaterThan(60);
    expect(Number(percent![1])).toBeLessThan(75);
    expect(html).toContain(translate(locale, 'scan.refine.model'));
    expect(html).not.toContain(translate(locale, 'scan.refine.no_reduction'));
  });

  it('is written in Ukrainian when the locale is Ukrainian', () => {
    expect(render('uk', scanned)).toContain('Перераховані пари');
    expect(render('en', scanned)).toContain('Pairs recalculated');
  });

  it.each(['en', 'uk'] as const)('says why nothing was recalculated in %s', (locale) => {
    expect(render(locale, null)).toContain(translate(locale, 'scan.refine.unavailable.no_registration'));
    const one = artifact([tag('G1', 1, [1, 0, 0])]);
    expect(render(locale, one)).toContain(translate(locale, 'scan.refine.unavailable.too_few_devices', { nodes: 'G1' }));
    expect(render(locale, null)).toContain('data-refinement-status="unavailable"');
  });

  it('says there is no reduction when the distances already agree with the scan', () => {
    const exact = artifact([tag('MAIN', 0, [0, 0, 0]), tag('G1', 1, [3, 0, 0])]);
    const html = render('en', exact);
    expect(html).toContain(translate('en', 'scan.refine.no_reduction'));
    expect(html).toContain('data-reduction-percent="none"');
  });
});
