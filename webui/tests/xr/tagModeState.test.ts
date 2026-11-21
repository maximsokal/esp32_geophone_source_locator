import { beforeAll, describe, expect, it } from 'vitest';
import { fuseTagObservations } from '../../src/xr/fiducial/tagObservationFusion';
import { confirmedTagIds, frameJumpTagIds, newFrameJumpNotices, newTagScanNotices, registrationOutcome, tagChipStatuses, tagScanNoticeText } from '../../src/xr/fiducial/tagModeState';
import { formatNumber } from '../../src/i18n/format';
import { loadUkrainianCatalog, translate, type I18nLocale } from '../../src/i18n';

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
      translate(locale, 'xr.tag.chip.views_seen', { node: 'MAIN', views: formatNumber(locale, 1), minimum: formatNumber(locale, 3) }),
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

describe('live scan notices', () => {
  beforeAll(loadUkrainianCatalog);
  const confirmedTag = (tagId: number) => Array.from({ length: 5 }, (_, index) => ({
    tagId, positionScan: [tagId + index * 0.001, 0, 0] as const, cameraPosition: [index * 0.3, 0, 0] as const, cameraForward: [0, 0, 1] as const,
  }));

  it('announces a device when its tag becomes confirmed, and names the pairs that are recalculated', () => {
    const first = fuseTagObservations(confirmedTag(0));
    expect(confirmedTagIds(first)).toEqual([0]);
    const notices = newTagScanNotices([], first);
    expect(notices).toEqual([{ tagId: 0, node: 'MAIN', others: [] }]);
    expect(tagScanNoticeText(notices[0], 'en')).toBe(translate('en', 'xr.tag.notice.first', { node: 'MAIN' }));

    const second = fuseTagObservations([...confirmedTag(0), ...confirmedTag(2)]);
    const next = newTagScanNotices(confirmedTagIds(first), second);
    expect(next).toEqual([{ tagId: 2, node: 'G2', others: ['MAIN'] }]);
    expect(tagScanNoticeText(next[0], 'en')).toBe('G2 scanned. The distance between the devices is being recalculated: G2\u2013MAIN.');
    expect(tagScanNoticeText(next[0], 'uk')).toContain('G2 відскановано');
    expect(tagScanNoticeText(next[0], 'uk')).toContain('G2\u2013MAIN');
  });

  it('does not repeat a notice and ignores tags that are only seen, not confirmed', () => {
    const seenOnly = fuseTagObservations([{ tagId: 1, positionScan: [1, 0, 0] }, { tagId: 1, positionScan: [1.001, 0, 0] }]);
    expect(confirmedTagIds(seenOnly)).toEqual([]);
    expect(newTagScanNotices([], seenOnly)).toEqual([]);
    const confirmed = fuseTagObservations(confirmedTag(1));
    expect(newTagScanNotices([1], confirmed)).toEqual([]);
  });

  it('announces two devices confirmed at once, each with the other', () => {
    const both = fuseTagObservations([...confirmedTag(0), ...confirmedTag(1)]);
    expect(newTagScanNotices([], both)).toEqual([
      { tagId: 0, node: 'MAIN', others: ['G1'] },
      { tagId: 1, node: 'G1', others: ['MAIN'] },
    ]);
  });
});

describe('frame jump of a tag (issue: coordinate jump without a reset event)', () => {
  const jumped = () => fuseTagObservations([
    ...[-0.157, -0.158, -0.164].map((x) => ({ tagId: 3, positionScan: [x, 1.03, -0.1] as [number, number, number] })),
    ...[-4.4, -4.395, -4.391].map((x) => ({ tagId: 3, positionScan: [x, 0.99, -0.19] as [number, number, number] })),
  ]);

  it.each(['en', 'uk'] as const)('shows the chip as a jump and announces it once, with the gap (%s)', (locale: I18nLocale) => {
    const fusion = jumped();
    expect(frameJumpTagIds(fusion)).toEqual([3]);
    const chip = tagChipStatuses(fusion, locale).find((entry) => entry.tagId === 3)!;
    expect(chip.status).toBe('frame-jump');
    expect(chip.text).toBe(translate(locale, 'xr.tag.chip.frame_jump', { node: chip.node }));
    const notices = newFrameJumpNotices([], fusion);
    expect(notices).toHaveLength(1);
    expect(notices[0].kind).toBe('jump');
    expect(notices[0].separationM).toBeGreaterThan(4);
    expect(tagScanNoticeText(notices[0], locale)).toContain(formatNumber(locale, 4.2));
    expect(newFrameJumpNotices([3], fusion)).toEqual([]);              /* already announced: no repeat */
    expect(newTagScanNotices([], fusion)).toEqual([]);                  /* a jumped tag is never announced as scanned */
  });
});
