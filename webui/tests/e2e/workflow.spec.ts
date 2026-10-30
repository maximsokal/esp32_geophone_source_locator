import { test, expect, catalog } from './fixtures/device';
import { gate, scenarioScreenshot } from './fixtures/scenarios';

test('one attached remote can enter the permitted initialization stage', async ({ page, api }) => {
  api.status.pods = [api.status.pods[0]];
  api.status.system.ready_pods = 1;
  Object.assign(api.status.workflow!, { step: 'SENSOR_CHECK', assigned_remotes: 1, allowed_actions: ['GO_CALIBRATION', 'BACK'] });
  api.once('POST /api/control', async route => {
    expect(route.request().postDataJSON()).toEqual({ action: 'workflow_command', command: 'GO_CALIBRATION', generation: 1 });
    Object.assign(api.status.workflow!, { step: 'CALIB_PREP', generation: 2, allowed_actions: ['SENSORS_PLACED', 'BACK'] });
    await route.fulfill({ json: { ok: true, workflow: api.status.workflow } });
  });
  await page.goto('/');
  await page.locator('[data-command="GO_CALIBRATION"]').click();
  await expect(page.getByRole('heading', { name: catalog.en['workflow.title.calib_prep'], exact: true })).toBeVisible();
  expect(api.controls).toHaveLength(1);
  await scenarioScreenshot(page, 'one-remote-init');
});

for (const reason of ['stale', 'not_allowed'] as const) {
  test(`409 ${reason} refreshes the authoritative stage without replay`, async ({ page, api }) => {
    api.expectedHttpErrors.add('/api/control');
    Object.assign(api.status.workflow!, { step: 'SENSOR_CHECK', allowed_actions: ['GO_CALIBRATION', 'BACK'] });
    api.once('POST /api/control', async route => {
      Object.assign(api.status.workflow!, { step: 'OBJECT', generation: 9, allowed_actions: ['START_MEASUREMENT', 'BACK'] });
      await route.fulfill({ status: 409, json: { ok: false, reason } });
    });
    await page.goto('/');
    await page.locator('[data-command="GO_CALIBRATION"]').click();
    await expect(page.getByRole('heading', { name: catalog.en['workflow.title.object'], exact: true })).toBeVisible();
    await expect(page.getByRole('status')).toContainText(catalog.en['workflow.state_changed']);
    expect(api.controls).toHaveLength(1);
    await scenarioScreenshot(page, `workflow-${reason}`);
  });
}

test('HTTP command failure leaves the stage intact and a manual retry can recover', async ({ page, api }) => {
  api.expectedHttpErrors.add('/api/control');
  Object.assign(api.status.workflow!, { step: 'SENSOR_CHECK', allowed_actions: ['GO_CALIBRATION', 'BACK'] });
  api.once('POST /api/control', async route => { await route.fulfill({ status: 500, json: { error: 'init_failed' } }); });
  await page.goto('/');
  await page.locator('[data-command="GO_CALIBRATION"]').click();
  await expect(page.getByRole('alert')).toContainText('500');
  await expect(page.getByRole('heading', { name: catalog.en['workflow.title.sensor_check'], exact: true })).toBeVisible();
  expect(api.controls).toHaveLength(1);
  api.once('POST /api/control', async route => {
    Object.assign(api.status.workflow!, { step: 'CALIB_PREP', generation: 2, allowed_actions: ['SENSORS_PLACED', 'BACK'] });
    await route.fulfill({ json: { ok: true, workflow: api.status.workflow } });
  });
  await page.locator('[data-command="GO_CALIBRATION"]').click();
  await expect(page.getByRole('heading', { name: catalog.en['workflow.title.calib_prep'], exact: true })).toBeVisible();
  expect(api.controls).toHaveLength(2);
});

for (const locale of ['en', 'uk'] as const) {
  test.describe(locale, () => {
    test.use({ appLocale: locale });
    test('record, stop, record again and arm/confirm deletion with current generations', async ({ page, api }) => {
      const t = catalog[locale];
      const responses = [
        ['START_RECORDING', 'RECORDING', ['STOP_RECORDING']],
        ['STOP_RECORDING', 'STOPPED', ['RECORD_AGAIN', 'DELETE_RECORDING', 'BACK']],
        ['RECORD_AGAIN', 'RECORD_READY', ['START_RECORDING', 'BACK']],
        ['START_RECORDING', 'RECORDING', ['STOP_RECORDING']],
        ['STOP_RECORDING', 'STOPPED', ['DELETE_RECORDING', 'BACK']],
        ['DELETE_RECORDING', 'RECORD_READY', ['START_RECORDING', 'BACK']],
      ] as const;
      let index = 0;
      Object.assign(api.status.workflow!, { step: 'RECORD_READY', allowed_actions: ['START_RECORDING', 'BACK'] });
      api.overrides.set('POST /api/control', async route => {
        const body = route.request().postDataJSON();
        const [command, step, allowed] = responses[index];
        expect(body).toEqual({ action: 'workflow_command', command, generation: index + 1 });
        index += 1;
        Object.assign(api.status.workflow!, { step, allowed_actions: [...allowed], generation: index + 1 });
        await route.fulfill({ json: { ok: true, workflow: api.status.workflow } });
      });
      await page.goto('/');
      for (const [command, step] of responses) {
        const button = page.locator(`[data-command="${command}"]`);
        if (command === 'DELETE_RECORDING') {
          const before = api.controls.length;
          await button.click();
          await expect(page.getByText(t['workflow.delete_confirm.prompt'], { exact: true })).toBeVisible();
          expect(api.controls).toHaveLength(before);
          await scenarioScreenshot(page, `${locale}-armed-delete`);
        }
        await button.click();
        await expect(page.getByRole('heading', { name: t[`workflow.title.${step.toLowerCase()}` as keyof typeof t], exact: true })).toBeVisible();
      }
      expect(api.controls).toHaveLength(6);
    });
  });
}

test('pending command disables its actions and navigating away does not duplicate it', async ({ page, api }) => {
  const pending = gate();
  Object.assign(api.status.workflow!, { step: 'SENSOR_CHECK', allowed_actions: ['GO_CALIBRATION', 'BACK'] });
  api.once('POST /api/control', async route => {
    await pending.promise;
    Object.assign(api.status.workflow!, { step: 'CALIB_PREP', generation: 2, allowed_actions: ['SENSORS_PLACED', 'BACK'] });
    await route.fulfill({ json: { ok: true, workflow: api.status.workflow } });
  });
  try {
    await page.goto('/');
    await page.locator('[data-command="GO_CALIBRATION"]').click();
    await expect(page.locator('[data-command="GO_CALIBRATION"]')).toBeDisabled();
    await page.getByRole('navigation').getByRole('button', { name: catalog.en['nav.home'], exact: true }).click();
    pending.release();
    await expect(page.getByRole('heading', { name: catalog.en['dashboard.title'], exact: true })).toBeVisible();
    await expect.poll(() => api.controls.length).toBe(1);
    await page.getByRole('navigation').getByRole('button', { name: catalog.en['workflow.nav'], exact: true }).click();
    await expect(page.getByRole('heading', { name: catalog.en['workflow.title.calib_prep'], exact: true })).toBeVisible();
    expect(api.controls).toHaveLength(1);
  } finally { pending.release(); }
});

for (const permission of ['granted', 'denied'] as const) {
  test(`phone GPS ${permission} produces one generation-scoped completion`, async ({ page, api, context }) => {
    Object.assign(api.status.workflow!, { step: 'GPS_CHOICE', allowed_actions: ['REQUEST_GPS', 'SKIP_GPS'] });
    if (permission === 'granted') {
      await context.grantPermissions(['geolocation']);
      await context.setGeolocation({ latitude: 50.45, longitude: 30.52, accuracy: 4 });
    } else await page.addInitScript(() => {
      Object.defineProperty(navigator, 'geolocation', { value: {
        getCurrentPosition: (_ok: unknown, fail: (e: { code: number; message: string }) => void) => fail({ code: 1, message: 'Permission denied by test' }),
      } });
    });
    api.overrides.set('POST /api/control', async route => {
      const body = route.request().postDataJSON();
      if (body.command === 'REQUEST_GPS') {
        expect(body.generation).toBe(1);
        Object.assign(api.status.workflow!, { step: 'GPS_PENDING', gps_status: 'pending', generation: 2, allowed_actions: ['BACK'] });
      } else {
        expect(body.command).toBe(permission === 'granted' ? 'GPS_ACCEPT' : 'GPS_CAPTURE_FAILED');
        expect(body.generation).toBe(2);
        if (permission === 'granted') expect(body).toMatchObject({ latitude: 50.45, longitude: 30.52, accuracy_m: 4 });
        Object.assign(api.status.workflow!, { step: 'LAYOUT_READY', gps_status: permission === 'granted' ? 'accepted' : 'failed', generation: 3, allowed_actions: ['BACK'] });
      }
      await route.fulfill({ json: { ok: true, workflow: api.status.workflow } });
    });
    await page.goto('/');
    await page.locator('[data-command="REQUEST_GPS"]').click();
    await expect(page.getByRole('heading', { name: catalog.en['workflow.title.layout_ready'], exact: true })).toBeVisible();
    expect(api.controls.map(c => c.command)).toEqual(['REQUEST_GPS', permission === 'granted' ? 'GPS_ACCEPT' : 'GPS_CAPTURE_FAILED']);
    await scenarioScreenshot(page, `gps-${permission}`);
  });
}


test('calibration, placement and radio ranging show the current stage and received progress', async ({ page, api }) => {
  const stages = [
    ['CALIB_PREP', 'workflow.title.calib_prep'],
    ['CALIBRATING', 'workflow.title.calibrating'],
    ['PLACEMENT', 'workflow.title.placement'],
    ['LAYOUT_READY', 'workflow.title.layout_ready'],
    ['RANGING', 'workflow.title.ranging'],
    ['RANGING_RESULT', 'workflow.title.ranging_result'],
  ] as const;
  for (const [step, title] of stages) {
    Object.assign(api.status.workflow!, { step, allowed_actions: ['BACK'] });
    Object.assign(api.status.ranging!.progress!, {
      active: step === 'RANGING', complete: step === 'RANGING_RESULT',
      success: step === 'RANGING_RESULT', pairs_done: 2, pairs_total: 3,
      pairs: [
        { node_a: 1, node_b: 2, state: 'valid', distance_mm: 1234, spread_mm: 0, confidence: 900, valid_samples: 3, attempts: 3, single_direction: false, direction_disagreement_mm: 0 },
        { node_a: 1, node_b: 3, state: 'missing', distance_mm: 0, spread_mm: 0, confidence: 0, valid_samples: 0, attempts: 3, single_direction: false, direction_disagreement_mm: 0 },
      ],
    });
    await page.goto('/');
    await expect(page.getByRole('heading', { name: catalog.en[title], exact: true })).toBeVisible();
    if (step === 'RANGING') {
      await expect(page.getByText(catalog.en['workflow.ranging.wait'])).toBeVisible();
      await expect(page.getByRole('progressbar', { name: catalog.en['workflow.ranging.pairs'] })).toHaveAttribute('value', '66.66666666666666');
    }
    if (step === 'RANGING_RESULT') {
      await expect(page.getByText(catalog.en['workflow.ranging.wait'])).toHaveCount(0);
      await expect(page.getByText('2 / 3', { exact: false })).toBeVisible();
      await expect(page.getByText('Nodes 1\u20132: 1,234 mm')).toBeVisible();
      await expect(page.getByText('Nodes 1\u20133: Not measured')).toBeVisible();
    }
  }
});


test.describe('Ukrainian ranging text', () => {
  test.use({ appLocale: 'uk' });
  test('shows progress and labels a missing pair', async ({ page, api }) => {
    Object.assign(api.status.workflow!, { step: 'RANGING', allowed_actions: ['BACK'] });
    Object.assign(api.status.ranging!.progress!, {
      active: true, complete: false, pairs_done: 1, pairs_total: 2,
      pairs: [{ node_a: 0, node_b: 1, state: 'missing', distance_mm: 0, spread_mm: 0,
        confidence: 0, valid_samples: 0, attempts: 3, single_direction: false, direction_disagreement_mm: 0 }],
    });
    await page.goto('/');
    await expect(page.getByText(catalog.uk['workflow.ranging.wait'])).toBeVisible();
    await expect(page.getByText(catalog.uk['workflow.ranging.pair_unavailable'], { exact: false })).toBeVisible();
    await expect(page.getByText('\u0412\u0443\u0437\u043b\u0438 0\u20131:', { exact: false })).toBeVisible();
  });
});


for (const outcome of ['success', 'partial', 'failed'] as const) {
  test(`ranging result displays ${outcome} and cable violation`, async ({ page, api }) => {
    Object.assign(api.status.workflow!, { step: 'RANGING_RESULT', allowed_actions: ['BACK', 'RETRY'] });
    Object.assign(api.status.ranging!.progress!, {
      active: false, complete: true, success: outcome === 'success', partial: outcome === 'partial',
      pairs_done: 2, pairs_total: 3,
      pairs: [
        { node_a: 0, node_b: 1, state: 'valid', distance_mm: 950, spread_mm: 0, confidence: 900, valid_samples: 3, attempts: 3, single_direction: false, direction_disagreement_mm: 0 },
        { node_a: 0, node_b: 2, state: 'cable_exceeded', distance_mm: 6001, spread_mm: 0, confidence: 900, valid_samples: 3, attempts: 3, single_direction: false, direction_disagreement_mm: 0 },
      ],
    });
    await page.goto('/');
    await expect(page.getByText(catalog.en[`workflow.ranging.${outcome}`], { exact: true })).toBeVisible();
    await expect(page.getByText('6,001 mm', { exact: false })).toContainText(catalog.en['workflow.ranging.cable_exceeded']);
    await expect(page.getByText('950 mm', { exact: false })).toBeVisible();
  });
}

for (const locale of ['en', 'uk'] as const) {
  test.describe(`manual distances ${locale}`, () => {
    test.use({ appLocale: locale });
    test('corrects an over-cable pair, preserves partial data and completes a missing pair', async ({ page, api }) => {
      const t = catalog[locale];
      Object.assign(api.status.workflow!, { step: 'RANGING_RESULT', generation: 12,
        layout_result: false, allowed_actions: ['RETRY', 'SET_DISTANCE', 'BACK'] });
      const pairs = [
        { node_a: 0, node_b: 1, state: 'valid', distance_mm: 900, spread_mm: 0, confidence: 900,
          valid_samples: 3, attempts: 3, single_direction: false, direction_disagreement_mm: 0, manual: false },
        { node_a: 0, node_b: 2, state: 'cable_exceeded', distance_mm: 6001, spread_mm: 0, confidence: 900,
          valid_samples: 3, attempts: 3, single_direction: false, direction_disagreement_mm: 0, manual: false },
        { node_a: 1, node_b: 2, state: 'missing', distance_mm: 0, spread_mm: 0, confidence: 0,
          valid_samples: 0, attempts: 3, single_direction: false, direction_disagreement_mm: 0, manual: false },
      ];
      Object.assign(api.status.ranging!.progress!, { active: false, complete: true, success: false,
        partial: true, started_at_ms: 100, updated_at_ms: 200, pairs, failure_reason: 'cable_exceeded:0-2' });
      api.overrides.set('POST /api/control', async route => {
        const body = route.request().postDataJSON();
        expect(body.command).toBe('SET_DISTANCE');
        expect(body.generation).toBe(api.status.workflow!.generation);
        expect(body.ranging_started_at_ms).toBe(100);
        expect(body.ranging_updated_at_ms).toBe(api.status.ranging!.progress!.updated_at_ms);
        if (body.distance_mm === 6001) {
          api.expectedHttpErrors.add('/api/control');
          await route.fulfill({ status: 400, json: { ok: false, error: 'ESP_ERR_INVALID_ARG' } });
          return;
        }
        const last = body.node_a === 1;
        expect(body).toMatchObject(last ? { node_a: 1, node_b: 2, distance_mm: 1000 }
          : { node_a: 0, node_b: 2, distance_mm: 1500 });
        Object.assign(pairs[last ? 2 : 1], { state: 'valid', distance_mm: body.distance_mm,
          manual: true, automatic_distance_mm: last ? 0 : 6001 });
        api.status.workflow!.generation += 1;
        Object.assign(api.status.workflow!, { layout_result: last,
          allowed_actions: [last ? 'CONFIRM' : 'RETRY', 'SET_DISTANCE', 'BACK'] });
        Object.assign(api.status.ranging!.progress!, { success: last, partial: !last,
          updated_at_ms: api.status.ranging!.progress!.updated_at_ms! + 1,
          failure_reason: last ? '' : 'pair_failed:1-2' });
        await route.fulfill({ json: { ok: true, workflow: api.status.workflow } });
      });
      await page.goto('/');
      await page.getByRole('button', { name: /0.2/ }).click();
      await expect(page.getByRole('spinbutton')).toHaveValue('6001');
      await scenarioScreenshot(page, `${locale}-manual-distance-entry`);
      await page.getByRole('button', { name: t['workflow.distance.save'], exact: true }).click();
      await expect(page.getByText(t['workflow.distance.rejected'], { exact: true })).toBeVisible();
      await scenarioScreenshot(page, `${locale}-manual-distance-rejected`);
      await page.getByRole('spinbutton').fill('1500');
      await page.getByRole('button', { name: t['workflow.distance.save'], exact: true }).click();
      await expect(page.getByRole('spinbutton')).toHaveCount(0);
      await expect(page.getByText(t['workflow.ranging.partial'], { exact: true })).toBeVisible();
      await expect(page.locator('[data-command="CONFIRM"]')).toHaveCount(0);
      await scenarioScreenshot(page, `${locale}-manual-distance-still-partial`);
      await page.getByRole('button', { name: /1.2/ }).click();
      await expect(page.getByRole('spinbutton')).toHaveValue('');
      await page.getByRole('button', { name: t['workflow.distance.cancel'], exact: true }).click();
      expect(api.controls).toHaveLength(2);
      await page.getByRole('button', { name: /1.2/ }).click();
      await page.getByRole('spinbutton').fill('1000');
      await page.getByRole('button', { name: t['workflow.distance.save'], exact: true }).click();
      await expect(page.getByText(t['workflow.ranging.success'], { exact: true })).toBeVisible();
      await expect(page.locator('[data-command="CONFIRM"]')).toBeVisible();
      await page.reload();
      await expect(page.getByText(t['workflow.ranging.success'], { exact: true })).toBeVisible();
      await expect(page.locator('.workflow-ranging')).toContainText(`(${t['workflow.distance.manual']})`);
      await scenarioScreenshot(page, `${locale}-manual-distance-complete`);
      expect(api.controls).toHaveLength(3);
    });
  });
}


// ESP32 exposes MAIN's execution counters; completed executions do not imply a successful result.
for (const locale of ['en', 'uk'] as const) {
  test.describe(`${locale} radio ranging percentage`, () => {
    test.use({ appLocale: locale });
    for (const [nodes, total, percent] of [[3, 6, '50'], [4, 12, '25'], [5, 20, '15']] as const) {
      test(`${nodes} nodes: 3 of ${total} windows is ${percent}%`, async ({ page, api }, testInfo) => {
        expect(total).toBe(nodes * (nodes - 1));
        Object.assign(api.status.workflow!, { step: 'RANGING', allowed_actions: ['BACK'] });
        Object.assign(api.status.ranging!.progress!, {
          active: true, complete: false, success: false, pairs_done: 3, pairs_total: total, pairs: [],
        });
        await page.goto('/');
        await expect(page.getByText(catalog[locale]['workflow.ranging.wait'])).toBeVisible();
        await expect(page.getByText(new RegExp(String.raw`3 / ${total} \(${percent}\s?%\)`))).toBeVisible();
        await expect(page.getByRole('progressbar', { name: catalog[locale]['workflow.ranging.pairs'] }))
          .toHaveAttribute('value', String((3 / total) * 100));
        await testInfo.attach(`ranging-${locale}-${nodes}-nodes`, {
          body: await page.screenshot({ fullPage: true }), contentType: 'image/png',
        });

        // Exercise the next /api/status poll without reloading the page.
        api.status.ranging!.progress!.pairs_done = total;
        await expect(page.getByText(new RegExp(String.raw`${total} / ${total} \(100\s?%\)`))).toBeVisible();
        await expect(page.getByRole('progressbar', { name: catalog[locale]['workflow.ranging.pairs'] }))
          .toHaveAttribute('value', '100');
        await expect(page.getByText(catalog[locale]['workflow.ranging.success'], { exact: true })).toHaveCount(0);

        Object.assign(api.status.workflow!, { step: 'RANGING_RESULT', allowed_actions: ['RETRY', 'BACK'] });
        Object.assign(api.status.ranging!.progress!, { active: false, complete: true, success: true });
        await expect(page.getByText(catalog[locale]['workflow.ranging.success'], { exact: true })).toBeVisible();
        await expect(page.getByText(new RegExp(String.raw`${total} / ${total} \(100\s?%\)`))).toBeVisible();
        expect(api.controls).toHaveLength(0);
      });
    }
  });
}
