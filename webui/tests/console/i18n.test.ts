import { describe, expect, it } from 'vitest';

import { translate, type I18nKey, type I18nLocale } from '../../src/i18n';

describe('i18n catalog translation', () => {
  it('falls back to English when the Ukrainian catalog entry is empty', () => {
    expect(translate('uk', 'nav.home')).toBe('Home');
  });

  it('substitutes provided parameters and leaves missing ones visible', () => {
    expect(translate('en', 'sensors.power_fault', { voltage: 4.2 })).toContain('4.2 V');
    expect(translate('en', 'sensors.power_fault')).toContain('{voltage}');
  });

  it('returns an unknown runtime key unchanged', () => {
    expect(translate('en', 'missing.key' as I18nKey)).toBe('missing.key');
  });

  it('uses English for an unknown runtime locale', () => {
    expect(translate('xx' as I18nLocale, 'nav.objects')).toBe('Objects');
  });
});
