import { test, expect, catalog } from './fixtures/device';
import { enterObjects, gate, objectCommands, scenarioScreenshot } from './fixtures/scenarios';

for (const locale of ['en', 'uk'] as const) {
  test.describe(locale, () => {
    test.use({ appLocale: locale });
    test('create, rename, select, activate, cancel and confirm deletion', async ({ page, api }) => {
      objectCommands(api);
      const t = catalog[locale];
      await enterObjects(page, api, locale);
      await page.getByRole('textbox', { name: t['objects.new_name'], exact: true }).fill('New survey');
      await page.getByRole('textbox', { name: t['objects.new_name'], exact: true }).press('Enter');
      await expect(page.getByRole('heading', { name: 'New survey', exact: true })).toBeVisible();
      await page.getByRole('textbox', { name: t['objects.rename'], exact: true }).fill('Renamed survey');
      await page.getByRole('button', { name: t['objects.rename'], exact: true }).click();
      await expect(page.getByRole('heading', { name: 'Renamed survey', exact: true })).toBeVisible();
      await page.locator('.object-row').filter({ hasText: 'OBJ_0001' }).click();
      await page.getByRole('button', { name: t['web.objects.set_active'], exact: true }).click();
      await expect(page.locator('.brand-button small')).toHaveText('Quarry');
      await page.locator('.object-row').filter({ hasText: 'OBJ_0002' }).click();
      page.once('dialog', async dialog => { expect(dialog.message()).toContain('Renamed survey'); await dialog.dismiss(); });
      await page.getByRole('button', { name: t['web.objects.delete'], exact: true }).click();
      expect(api.controls.filter(c => c.action === 'delete_object')).toHaveLength(0);
      page.once('dialog', dialog => dialog.accept());
      await page.getByRole('button', { name: t['web.objects.delete'], exact: true }).click();
      await expect(page.locator('.object-row')).toHaveCount(1);
      expect(api.controls.map(c => c.action)).toEqual(['create_object', 'rename_object', 'activate_object', 'delete_object']);
      await scenarioScreenshot(page, `${locale}-objects-complete`);
    });
  });
}

test('a rejected create retains the typed name and cannot show a created object', async ({ page, api }) => {
  api.expectedHttpErrors.add('/api/control');
  api.once('POST /api/control', async route => { await route.fulfill({ status: 400, json: { error: 'invalid_name' } }); });
  await enterObjects(page, api);
  const name = page.getByRole('textbox', { name: catalog.en['objects.new_name'], exact: true });
  await name.fill('Retry this survey');
  await name.press('Enter');
  await expect(page.getByRole('alert')).toContainText('400');
  await expect(name).toHaveValue('Retry this survey');
  await expect(page.locator('.object-row')).toHaveCount(1);
  expect(api.controls).toHaveLength(1);
  await scenarioScreenshot(page, 'create-rejected');
});

test('a pending create admits one write even after repeated pointer and Enter input', async ({ page, api }) => {
  const pending = gate();
  objectCommands(api);
  const handle = api.overrides.get('POST /api/control')!;
  api.once('POST /api/control', async route => { await pending.promise; await handle(route); });
  try {
    await enterObjects(page, api);
    const name = page.getByRole('textbox', { name: catalog.en['objects.new_name'], exact: true });
    await name.fill('Pending survey');
    await name.press('Enter');
    await expect(page.getByRole('button', { name: catalog.en['objects.create'], exact: true })).toBeDisabled();
    await name.press('Enter');
    await name.press('Enter');
    expect(api.controls).toHaveLength(1);
    pending.release();
    await expect(page.getByRole('heading', { name: 'Pending survey', exact: true })).toBeVisible();
    expect(api.controls).toHaveLength(1);
  } finally { pending.release(); }
});

test('lost create reply reconciles the committed object without replaying the write', async ({ page, api }) => {
  api.expectedHttpErrors.add('/api/control');
  api.once('POST /api/control', async route => {
    const store = api.status.object_store!;
    store.objects.push({ ...structuredClone(store.objects[0]), id: 'OBJ_0002', name: 'Committed survey', active: true });
    store.objects[0].active = false;
    store.active_id = 'OBJ_0002';
    await route.abort('connectionreset');
  });
  await enterObjects(page, api);
  await page.getByRole('textbox', { name: catalog.en['objects.new_name'], exact: true }).fill('Committed survey');
  await page.getByRole('button', { name: catalog.en['objects.create'], exact: true }).click();
  await expect(page.getByRole('alert')).toContainText('request failed');
  await expect(page.getByRole('heading', { name: 'Committed survey', exact: true })).toBeVisible();
  expect(api.controls).toHaveLength(1);
  await expect(page.getByRole('alert')).toHaveCount(0);
  await scenarioScreenshot(page, 'lost-reply-reconciled');
});

test('late sessions of a previous selection cannot replace the current object', async ({ page, api }) => {
  api.status.object_store!.objects.push({ ...structuredClone(api.status.object_store!.objects[0]), id: 'OBJ_0002', name: 'Second survey', active: false });
  const pending = gate();
  api.once('GET /api/status?sessions=OBJ_0001', async route => {
    const old = structuredClone(api.status);
    await pending.promise;
    await route.fulfill({ json: old });
  });
  api.overrides.set('GET /api/status?sessions=OBJ_0002', async route => {
    await route.fulfill({ json: { ...api.status, recording_sessions: { available: true, object_id: 'OBJ_0002', sessions: [], error: '' } } });
  });
  try {
    await enterObjects(page, api);
    await expect.poll(() => api.requests.includes('GET /api/status?sessions=OBJ_0001')).toBe(true);
    await page.locator('.object-row').filter({ hasText: 'OBJ_0002' }).click();
    pending.release();
    await expect.poll(() => api.requests.includes('GET /api/status?sessions=OBJ_0002')).toBe(true);
    await expect(page.getByRole('heading', { name: 'Second survey', exact: true })).toBeVisible();
    await expect(page.locator('.session-row')).toHaveCount(0);
    await expect(page.getByText(catalog.en['objects.no_sessions'], { exact: true })).toBeVisible();
    expect(api.controls).toHaveLength(0);
  } finally { pending.release(); }
});
