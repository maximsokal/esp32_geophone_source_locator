import { describe, expect, it } from 'vitest';
import { fuseTagObservations } from '../../src/xr/fiducial/tagObservationFusion';
import { registrationOutcome, tagChipStatuses } from '../../src/xr/fiducial/tagModeState';
import { formatNumber } from '../../src/i18n/format';
import { translate, type I18nLocale } from '../../src/i18n';

describe('tag-mode status', () => {
  it.each(['en', 'uk'] as const)('maps fused observations to localized chip labels in %s', (locale: I18nLocale) => {
    const fusion = fuseTagObservations([
      { tagId: 0, positionScan: [0, 0, 0] }, { tagId: 0, positionScan: [0.001, 0, 0] },
      ...Array.from({ length: 5 }, (_, i) => ({
        tagId: 2,
        positionScan: [1 + i * 0.001, 0, 0] as const,
        cameraPosition: [i * 0.3, 0, 0] as const,
        cameraForward: [0, 0, 1] as const,
      })),
    ]);
    const chips = tagChipStatuses(fusion, locale);
    expect(chips.map(({ tagId, node, status }) => ({ tagId, node, status }))).toEqual([
      { tagId: 0, node: 'MAIN', status: 'seen' },
      { tagId: 1, node: 'G1', status: 'not-seen' },
      { tagId: 2, node: 'G2', status: 'confirmed' },
      { tagId: 3, node: 'G3', status: 'not-seen' },
      { tagId: 4, node: 'G4', status: 'not-seen' },
    ]);
    expect(chips.map(({ text }) => text)).toEqual([
      translate(locale, 'xr.tag.chip.seen', { node: 'MAIN', inliers: formatNumber(locale, 2), minimum: formatNumber(locale, 5) }),
      translate(locale, 'xr.tag.chip.not_seen', { node: 'G1' }),
      translate(locale, 'xr.tag.chip.confirmed', { node: 'G2' }),
      translate(locale, 'xr.tag.chip.not_seen', { node: 'G3' }),
      translate(locale, 'xr.tag.chip.not_seen', { node: 'G4' }),
    ]);
  });

  it.each(['en', 'uk'] as const)('localizes final registration results in %s and preserves unknown reason codes', (locale: I18nLocale) => {
    expect(registrationOutcome(null, locale)).toBe(translate(locale, 'xr.tag.registration.none'));
    expect(registrationOutcome({ valid: true, tier: 'full', reason: null }, locale)).toBe(translate(locale, 'xr.tag.registration.valid', { tier: translate(locale, 'xr.tag.tier.full') }));
    expect(registrationOutcome({ valid: false, tier: null, reason: 'too_few_remotes' }, locale)).toBe(translate(locale, 'xr.tag.registration.failed', { reason: translate(locale, 'xr.tag.reason.too_few_remotes') }));
    expect(registrationOutcome({ valid: false, tier: null, reason: 'future_reason' }, locale)).toBe(translate(locale, 'xr.tag.registration.failed', { reason: 'future_reason' }));
  });
});
