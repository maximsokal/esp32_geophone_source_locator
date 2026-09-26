import { describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import LevelingCard from '../../src/features/sensors/LevelingCard';
import type { RecordingGateStatus } from '../../src/domain/device';

/* MAX-114: which geophone blocks recording, and by how much. */

describe('LevelingCard', () => {
  it('lists each node with state and tilt, marking the blocking ones', () => {
    const gate: RecordingGateStatus = {
      available: true,
      assigned_count: 3,
      accepted_count: 1,
      allowed: false,
      reason: 'Level every connected geophone before recording.',
      nodes: [
        { channel: 0, node_id: 1, state: 'level', tilt_deg: 0.4 },
        { channel: 2, node_id: 3, state: 'tilted', tilt_deg: 12.34 },
        { channel: 5, node_id: 6, state: 'no_data' },
      ],
    };
    const html = renderToStaticMarkup(createElement(LevelingCard, { gate, pods: [], onCalibrateHeights: async () => true }));
    expect(html).toContain('1/3 LEVEL');
    expect(html).toContain('Level every connected geophone');
    expect(html).toContain('TILTED');
    expect(html).toContain('12.3°');
    expect(html).toContain('NO DATA');
    expect(html.match(/class="blocking"/g)?.length).toBe(2);
    expect(html.match(/class="ok"/g)?.length).toBe(1);
    /* Channels are shown 1-based like the pod cards. */
    expect(html).toContain('<td>6</td>');
  });

  it('explains an unavailable topology and keeps calibration available without gate data', () => {
    const html = renderToStaticMarkup(createElement(LevelingCard, {
      gate: { available: false, error: 'ESP_ERR_INVALID_STATE' },
      pods: [],
      onCalibrateHeights: async () => true,
    }));
    expect(html).toContain('UNAVAILABLE');
    expect(html).toContain('ESP_ERR_INVALID_STATE');
    expect(renderToStaticMarkup(createElement(LevelingCard, {
      gate: undefined,
      pods: [],
      onCalibrateHeights: async () => true,
    }))).toContain('Calibrate heights');
  });
});
