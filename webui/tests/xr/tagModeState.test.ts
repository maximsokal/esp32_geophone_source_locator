import { describe, expect, it } from 'vitest';
import { fuseTagObservations } from '../../src/xr/fiducial/tagObservationFusion';
import { registrationOutcome, tagChipStatuses } from '../../src/xr/fiducial/tagModeState';

describe('tag-mode status', () => {
  it('maps fused observations to not seen, seen n/5, and confirmed chips', () => {
    const fusion = fuseTagObservations([
      { tagId: 0, positionScan: [0, 0, 0] }, { tagId: 0, positionScan: [0.001, 0, 0] },
      ...Array.from({ length: 5 }, (_, i) => ({ tagId: 2, positionScan: [1 + i * 0.001, 0, 0] as const })),
    ]);
    expect(tagChipStatuses(fusion)).toEqual([
      { tagId: 0, node: 'MAIN', text: 'MAIN seen 2/5', status: 'seen' },
      { tagId: 1, node: 'G1', text: 'G1 not seen', status: 'not-seen' },
      { tagId: 2, node: 'G2', text: 'G2 confirmed', status: 'confirmed' },
      { tagId: 3, node: 'G3', text: 'G3 not seen', status: 'not-seen' },
      { tagId: 4, node: 'G4', text: 'G4 not seen', status: 'not-seen' },
    ]);
  });

  it('maps final registration results to a tier, failure reason, or no attempt', () => {
    expect(registrationOutcome(null)).toBe('No registration attempted.');
    expect(registrationOutcome({ valid: true, tier: 'full', reason: null })).toBe('Valid full tier.');
    expect(registrationOutcome({ valid: false, tier: null, reason: 'too_few_remotes' })).toBe('Registration failed: too few remotes.');
  });
});
