import { afterEach, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';

const lazyUk = vi.hoisted(() => {
  let resolve!: (module: { uk: Record<string, string> }) => void;
  const promise = new Promise<{ uk: Record<string, string> }>((done) => { resolve = done; });
  return { promise, resolve: (module: { uk: Record<string, string> }) => resolve(module) };
});
vi.mock('../../src/i18n/catalog.uk.gen', async () => await lazyUk.promise);

import { formatNumber } from '../../src/i18n/format';
import { LocaleProvider, translate, useLocale, type I18nLocale } from '../../src/i18n';

afterEach(() => vi.unstubAllGlobals());

it('shows English for a stored UK preference and settles atomically on the last selection', async () => {
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
      createElement('span', { id: 'decimal' }, formatNumber(state.locale, 1.5)),
      createElement('select', { id: 'locale', value: state.selectedLocale, 'aria-busy': state.localeLoading }),
      state.localeLoading ? createElement('span', { role: 'status' }, translate(state.locale, 'web.locale_loading')) : null,
    );
  }

  let renderer!: ReactTestRenderer;
  await act(async () => { renderer = create(createElement(LocaleProvider, null, createElement(Probe))); });
  expect(renderer.root.findByProps({ id: 'home' }).children).toEqual([translate('en', 'nav.home')]);
  expect(renderer.root.findByProps({ id: 'decimal' }).children).toEqual([formatNumber('en', 1.5)]);
  expect(renderer.root.findByProps({ id: 'locale' }).props).toMatchObject({ value: 'uk', 'aria-busy': true });

  act(() => setLocale('en'));
  act(() => setLocale('uk'));
  expect(renderer.root.findByProps({ id: 'home' }).children).toEqual([translate('en', 'nav.home')]);
  expect(renderer.root.findByProps({ id: 'locale' }).props).toMatchObject({ value: 'uk', 'aria-busy': true });

  await act(async () => {
    lazyUk.resolve({ uk: { 'nav.home': 'Домівка' } });
    await lazyUk.promise;
    await new Promise((resolve) => setTimeout(resolve, 0));
  });

  expect(renderer.root.findByProps({ id: 'home' }).children).toEqual(['Домівка']);
  expect(renderer.root.findByProps({ id: 'decimal' }).children).toEqual([formatNumber('uk', 1.5)]);
  expect(renderer.root.findByProps({ id: 'locale' }).props).toMatchObject({ value: 'uk', 'aria-busy': false });
  expect(renderer.root.findAllByProps({ role: 'status' })).toHaveLength(0);
  act(() => renderer.unmount());
});
