import { describe, expect, it } from 'vitest';
import type { DeviceStatus, WorkflowPlacementStatus } from '../../src/domain/device';
import { normalizeDeviceStatus } from '../../src/api/deviceStatusSchema';
import {
  placementBlocker,
  placementReasonKey,
  workflowViewModel,
} from '../../src/features/workflow/workflowViewModel';
import { translate } from '../../src/i18n';
import { fullDeviceStatus } from './fixtures/deviceStatusFull';

const PLACEMENT_REASONS = [
  'stable', 'collecting', 'range_exceeded', 'not_vertical', 'device_status_busy', 'main_reference',
  'main_barometer', 'node_stale', 'node_environment', 'node_reference', 'sample_stale',
  'overflow_blocked', 'clock_backward',
];

function placementStatus(allowed: string[], placement: Partial<WorkflowPlacementStatus> = {}): DeviceStatus {
  const status = fullDeviceStatus();
  Object.assign(status.workflow!, { step: 'PLACEMENT', allowed_actions: allowed });
  Object.assign(status.workflow!.placement, placement);
  return status;
}

describe('placement stability on the web (parity with the TFT)', () => {
  it('keeps Continue visible but disabled while the firmware has not allowed it', () => {
    const view = workflowViewModel(placementStatus(['BACK']));
    expect(view.actions.map((action) => [action.command, action.disabled === true])).toEqual([
      ['PLACEMENT_CONFIRMED', true],
      ['BACK', false],
    ]);
    expect(view.actions[0].primary).toBe(true);
  });

  it('offers Continue normally once the firmware allows it', () => {
    const view = workflowViewModel(placementStatus(['PLACEMENT_CONFIRMED', 'BACK']));
    expect(view.actions.map((action) => [action.command, action.disabled === true])).toEqual([
      ['PLACEMENT_CONFIRMED', false],
      ['BACK', false],
    ]);
  });

  it('does not invent a Continue button on other steps', () => {
    const status = placementStatus(['BACK']);
    Object.assign(status.workflow!, { step: 'LAYOUT_READY' });
    expect(workflowViewModel(status).actions.map((action) => action.command)).toEqual(['BACK']);
  });

  it('names the first node that is not stable', () => {
    const base = fullDeviceStatus().workflow!.placement.per_node[0];
    const placement: WorkflowPlacementStatus = {
      all_stable: false, max_range_mm: 50, min_samples: 3,
      per_node: [
        { ...base, node: 'MAIN', stable: true, reason: 'stable' },
        { ...base, node: 'G1', stable: false, reason: 'not_vertical', samples: 0, range_mm: null },
        { ...base, node: 'G2', stable: false, reason: 'range_exceeded', samples: 5, range_mm: 84 },
      ],
    };
    expect(placementBlocker(placement)?.node).toBe('G1');
    expect(placementBlocker({ ...placement, per_node: placement.per_node.slice(0, 1) })).toBeNull();
  });

  it.each(['en', 'uk'] as const)('has a %s text for every firmware reason', (locale) => {
    for (const reason of PLACEMENT_REASONS) {
      const key = placementReasonKey(reason);
      expect(key, reason).not.toBeNull();
      const text = translate(locale, key!);
      expect(text.length, reason).toBeGreaterThan(0);
      expect(text, reason).not.toBe(key);
    }
    expect(placementReasonKey('a_reason_from_a_newer_firmware')).toBeNull();
  });

  it('requires the reason, sample count and spread from the firmware', () => {
    const valid = JSON.parse(JSON.stringify(placementStatus(['BACK']))) as Record<string, any>;
    expect(() => normalizeDeviceStatus(valid)).not.toThrow();
    for (const field of ['reason', 'samples', 'range_mm']) {
      const broken = JSON.parse(JSON.stringify(valid)) as Record<string, any>;
      delete broken.workflow.placement.per_node[0][field];
      expect(() => normalizeDeviceStatus(broken), field).toThrow();
    }
    for (const field of ['max_range_mm', 'min_samples']) {
      const broken = JSON.parse(JSON.stringify(valid)) as Record<string, any>;
      delete broken.workflow.placement[field];
      expect(() => normalizeDeviceStatus(broken), field).toThrow();
    }
  });
});
