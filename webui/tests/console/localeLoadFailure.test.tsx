import { afterEach, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';

const lazyUk = vi.hoisted(() => {
  let reject!: (reason: Error) => void;
  const promise = new Promise<{ uk: Record<string, string> }>((_resolve, fail) => { reject = fail; });
  return { promise, reject: (reason: Error) => reject(reason) };
});
vi.mock('../../src/i18n/catalog.uk.gen', async () => await lazyUk.promise);

import { LocaleProvider, translate, useLocale, type I18nLocale } from '../../src/i18n';

afterEach(() => vi.unstubAllGlobals());

it('keeps English active and shows a non-blocking notice when the UK chunk fails', async () => {
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  vi.stubGlobal('window', {
    localStorage: {
      getItem: () => JSON.stringify({ schema: 1, locale: 'uk' }),
      setItem: vi.fn(),
    },
  });

  let setLocale!: (locale: I18nLocale) => void;
  function Probe() {
    const state = useLocale();
    setLocale = state.setLocale;
    return createElement('section', null,
      createElement('span', { id: 'home' }, translate(state.locale, 'nav.home')),
      createElement('select', { id: 'locale', value: state.selectedLocale, onChange: (event: { target: { value: I18nLocale } }) => state.setLocale(event.target.value) }),
      state.localeLoadFailed ? createElement('span', { id: 'notice', role: 'status' }, translate(state.locale, 'web.locale_load_failed')) : null,
    );
  }

  let renderer!: ReactTestRenderer;
  await act(async () => { renderer = create(createElement(LocaleProvider, null, createElement(Probe))); });
  expect(renderer.root.findByProps({ id: 'home' }).children).toEqual([translate('en', 'nav.home')]);

  await act(async () => {
    lazyUk.reject(new Error('chunk unavailable'));
    await new Promise((resolve) => setTimeout(resolve, 0));
  });

  expect(renderer.root.findByProps({ id: 'home' }).children).toEqual([translate('en', 'nav.home')]);
  expect(renderer.root.findByProps({ id: 'locale' }).props.value).toBe('en');
  expect(renderer.root.findByProps({ id: 'notice' }).children).toEqual([translate('en', 'web.locale_load_failed')]);
  expect(() => act(() => setLocale('en'))).not.toThrow();
  act(() => renderer.unmount());
});
