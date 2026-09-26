import { describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import { RecordingEvidenceView } from '../../src/features/objects/RecordingEvidenceCard';
import { buildRecordingEvidence } from '../../src/domain/recordingEvidence';

/* MAX-169: the evidence card states facts or "unknown", never guesses. */

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
    const html = renderToStaticMarkup(createElement(RecordingEvidenceView, { evidence }));
    expect(html).toContain('3/4 (Pod 1, 2, 3)');
    expect(html).toContain('planar only (3 REMOTEs)');
    expect(html).toContain('2/3');
    expect(html).toContain('available (4 KiB, occupancy artifact saved), not registered to the sensors');
    expect(html).toContain('not calculated');
    expect(html).toContain('<dt>AprilTag anchor</dt><dd>none</dd>');
    expect(html).toContain('0.03 m');
  });

  it('says unknown while nothing has loaded', () => {
    const html = renderToStaticMarkup(createElement(RecordingEvidenceView, { evidence: buildRecordingEvidence(null, null, null) }));
    expect(html.match(/unknown/g)?.length).toBeGreaterThanOrEqual(4);
  });
});
