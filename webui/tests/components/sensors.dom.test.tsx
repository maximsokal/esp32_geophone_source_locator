import { screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import SensorsFeature from '../../src/features/sensors/SensorsFeature';
import { translate } from '../../src/i18n';
import { readyStatus, renderUi } from './setup';

describe.each(['en', 'uk'] as const)('sensors DOM %s', (locale) => {
  const t = (key: Parameters<typeof translate>[1]) => translate(locale, key);

  it('shows four distinct pod cards, including missing nodes and valid zero values', async () => {
    const status = readyStatus();
    status.pods[0].state = 'READY';
    renderUi(<SensorsFeature status={status} object={status.object_store!.objects[0]}
      error={null} onCalibrateHeights={vi.fn(async () => true)} />, locale);
    expect(screen.getByRole('heading', { name: t('sensors.acquisition_four_remotes') })).toBeTruthy();
    for (let index = 1; index <= 4; index += 1) {
      const heading = screen.getByRole('heading', { name: translate(locale, 'sensors.pod', { number: index }) });
      const card = within(heading.closest('article')!);
      if (index <= 2) expect(card.getByText(/0[.,]0° \/ 0[.,]0°/)).toBeTruthy();
      else expect(card.getByText(t('sensors.pod_state.disconnected'))).toBeTruthy();
    }
    await screen.findByText(t('nrf_log.panel.no_nodes'));
  });

  it('keeps calibration keyboard-accessible with one remote and shows reference mismatch', async () => {
    const user = userEvent.setup();
    const status = readyStatus();
    status.pods = status.pods.slice(0, 1);
    status.pods[0].height_reference!.generation = 2;
    const calibrate = vi.fn(async () => true);
    renderUi(<SensorsFeature status={status} object={null} error={null} onCalibrateHeights={calibrate} />, locale);
    const button = screen.getByRole('button', { name: t('leveling.calibrate_heights') });
    await user.tab();
    expect(document.activeElement).toBe(button);
    await user.keyboard('{Enter}');
    expect(calibrate).toHaveBeenCalledTimes(1);
    expect(screen.getByRole('status').textContent).toBe(t('leveling.generations_differ'));
  });

  it('exposes transport/power failures as alerts instead of hiding them in ready data', () => {
    const status = readyStatus();
    status.acquisition.serial!.power_fault_latched = true;
    status.acquisition.serial!.power_gate = 'overcurrent';
    renderUi(<SensorsFeature status={status} object={null} error="fixture transport failure"
      onCalibrateHeights={vi.fn(async () => false)} />, locale);
    const alerts = screen.getAllByRole('alert').map((node) => node.textContent).join('\n');
    expect(alerts).toContain('fixture transport failure');
    expect(alerts).toContain(t('sensors.overcurrent_alert'));
  });
});
