import { beforeAll, describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import { loadUkrainianCatalog, LocaleProvider, translate, type I18nLocale } from '../../src/i18n';
import { formatDistance, formatNumber } from '../../src/i18n/format';
import { RecordingEvidenceView } from '../../src/features/objects/RecordingEvidenceCard';
import { buildRecordingEvidence } from '../../src/domain/recordingEvidence';

/* MAX-169: the evidence card states facts or "unknown", never guesses. */

beforeAll(async () => { await loadUkrainianCatalog(); });

describe('RecordingEvidenceView', () => {
  it('shows participation, policy, scan and localization status of one recording', () => {
    const evidence = buildRecordingEvidence(
      {
        object_id: 'o', session_id: 's',
        geometry: { available: true, pods: [
          { pod: 0, computed: true, leveled: true, x_mm: 0, y_mm: 0, z_mm: 0 },
          { pod: 1, computed: true, leveled: true, x_mm: 4000, y_mm: 0, z_mm: 0, residual_mm: 30 },
          { pod: 2, computed: true, leveled: true, x_mm: 0, y_mm: 3000, z_mm: 0 },
          { pod: 3, computed: true, leveled: false, x_mm: 3000, y_mm: 3000, z_mm: 0 },
        ] },
        cs_pairs: { available: false, pairs: [] }, picks: [], runs: [],
      },
      { present: true, bytes: 4096 },
      { present: true, bytes: 100 },
    );
    const html = renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: 'en' },
      createElement(RecordingEvidenceView, { evidence })));
    const remotes = [1, 2, 3].map((number) => translate('en', 'report.remote_node', { number: formatNumber('en', number) })).join(', ');
    expect(html).toContain(`${translate('en', 'report.remote_count', { count: formatNumber('en', 3)})} (${remotes})`);
    expect(html).toContain(translate('en', 'evidence.policy.planar'));
    expect(html).toContain(`${formatNumber('en', 2)}/${formatNumber('en', 3)}`);
    expect(html).toContain(translate('en', 'evidence.scan.available', {
      size: `${formatNumber('en', 4)} KiB`,
      occupancy: translate('en', 'evidence.occupancy_saved'),
      registration: translate('en', 'evidence.not_registered'),
    }));
    expect(html).toContain(translate('en', 'evidence.localization.not_calculated'));
    expect(html).toContain(`<dt>${translate('en', 'evidence.apriltag_anchor')}</dt><dd>${translate('en', 'evidence.anchor_none')}</dd>`);
    expect(html).toContain(formatDistance('en', 0.03));
  });

  it('says unknown while nothing has loaded', () => {
    const html = renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: 'en' },
      createElement(RecordingEvidenceView, { evidence: buildRecordingEvidence(null, null, null) })));
    expect(html).toContain(`<dt>${translate('en', 'evidence.remote_nodes')}</dt><dd>${translate('en', 'evidence.policy.unknown')}</dd>`);
    expect(html).toContain(`<dt>${translate('en', 'evidence.localization_policy')}</dt><dd>${translate('en', 'evidence.policy.unknown')}</dd>`);
    expect(html).toContain(`<dt>${translate('en', 'evidence.scan')}</dt><dd>${translate('en', 'evidence.scan.unknown')}</dd>`);
    expect(html).toContain(`<dt>${translate('en', 'evidence.localization')}</dt><dd>${translate('en', 'evidence.localization.unknown')}</dd>`);
  });

  it.each(['en', 'uk'] as const)('renders missing z as unknown and a valid zero as zero in %s', (locale: I18nLocale) => {
    const evidence = buildRecordingEvidence({
      object_id: 'o', session_id: 's',
      geometry: { available: true, pods: [
        { pod: 0, computed: true, leveled: true, x_mm: 0, y_mm: 0 },
        { pod: 1, computed: true, leveled: true, x_mm: 1000, y_mm: 0, z_mm: 0 },
      ] },
      cs_pairs: { available: false, pairs: [] }, picks: [], runs: [],
    }, null, null);
    const html = renderToStaticMarkup(createElement(LocaleProvider, { initialLocale: locale },
      createElement(RecordingEvidenceView, { evidence })));
    const zero = formatNumber(locale, 0, { minimumFractionDigits: 1, maximumFractionDigits: 1 });
    const unknown = translate(locale, 'evidence.policy.unknown');
    const remote = translate(locale, 'report.remote_node', { number: formatNumber(locale, 1) });
    expect(html).toContain(`MAIN ${zero}/${zero}/${unknown}`);
    expect(html).toContain(`${remote} ${formatNumber(locale, 1, { minimumFractionDigits: 1, maximumFractionDigits: 1 })}/${zero}/${zero}`);
  });
});
