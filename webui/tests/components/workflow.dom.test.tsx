import type { ComponentProps } from 'react';
import { act, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import WorkflowPanel from '../../src/features/workflow/WorkflowPanel';
import type { WorkflowCommandResult } from '../../src/api/deviceApi';
import { translate } from '../../src/i18n';
import { formatPercent } from '../../src/i18n/format';
import { readyStatus, renderUi, respondJson } from './setup';

vi.mock('../../src/features/seismic/LiveSeismicPanel', async () => {
  const { createElement } = await import('react');
  return { default: () => createElement('div', { 'data-testid': 'live-seismic' }, 'LIVE_SEISMIC_PANEL') };
});

type Props = ComponentProps<typeof WorkflowPanel>;
function panelProps(step: string, actions: string[]): Props {
  const status = readyStatus();
  Object.assign(status.workflow!, { step, allowed_actions: actions, generation: 7 });
  return {
    status, objectName: 'Quarry',
    sendCommand: vi.fn<Props['sendCommand']>().mockResolvedValue({ ok: true, workflow: status.workflow! }),
    onRefreshStatus: vi.fn(async () => true), onWorkflowUpdate: vi.fn(), onOpenScanner: vi.fn(),
  };
}

describe.each(['en', 'uk'] as const)('workflow DOM %s', (locale) => {
  const t = (key: Parameters<typeof translate>[1]) => translate(locale, key);

  function placementProps(allowed: string[], allStable: boolean): Props {
    const props = panelProps('PLACEMENT', allowed);
    props.status!.workflow!.placement = {
      all_stable: allStable,
      max_range_mm: 100,
      min_samples: 8,
      per_node: [
        { node: 'MAIN', vertical: true, height_rel_main_mm: 0, altitude_asl_m: 10, stable: true,
          reason: 'stable', samples: 30, range_mm: 0 },
        { node: 'G2', vertical: true, height_rel_main_mm: 84, altitude_asl_m: 10.1, stable: allStable,
          reason: allStable ? 'stable' : 'range_exceeded', samples: 9, range_mm: allStable ? 2 : 134 },
      ],
    };
    return props;
  }

  it('shows Continue disabled with the blocking node and reason while placement is not stable', async () => {
    const user = userEvent.setup();
    const props = placementProps(['BACK'], false);
    renderUi(<WorkflowPanel {...props} />, locale);
    const proceed = screen.getByRole('button', { name: t('workflow.action.placement_confirmed') }) as HTMLButtonElement;
    expect(proceed.disabled).toBe(true);
    expect(proceed.getAttribute('aria-disabled')).toBe('true');
    const blocked = screen.getByTestId('placement-blocked');
    expect(blocked.textContent).toContain('G2');
    expect(blocked.textContent).toContain(t('workflow.placement.reason.range_exceeded'));
    // 134 mm of uncertainty is shown rounded up to whole centimetres, in the unit of the locale.
    expect(blocked.textContent).toContain(locale === 'uk' ? '14 см' : '14 cm');
    expect(blocked.textContent).not.toMatch(/mm|мм/);
    await user.click(proceed);
    expect(props.sendCommand).not.toHaveBeenCalled();
    // The per-node reason is listed too.
    expect(document.querySelector('[data-reason="range_exceeded"]')).not.toBeNull();
  });

  it('shows heights and uncertainty in centimetres, rounded as on the TFT, never in millimetres', () => {
    const props = placementProps(['BACK'], false);
    const nodes = props.status!.workflow!.placement.per_node;
    nodes[0].height_rel_main_mm = 0;
    nodes[1].height_rel_main_mm = 1234;     /* 123 cm */
    nodes[1].range_mm = 101;                /* an uncertainty rounds up: 11 cm against the 10 cm limit */
    renderUi(<WorkflowPanel {...props} />, locale);
    const unit = locale === 'uk' ? 'см' : 'cm';
    const text = document.body.textContent ?? '';
    expect(text).toContain(`123 ${unit}`);
    expect(text).toContain(`11 ${unit}`);
    expect(text).toContain(`0 ${unit}`);
    expect(text).not.toContain('1234');
    expect(text).not.toMatch(/\d\s?(mm|мм)/);
    // The rule the operator reads: 10 cm, taken from the limit the firmware reports.
    expect(text).toContain(locale === 'uk' ? '10 см' : '10 cm');
  });

  it('enables Continue and sends it once every node is stable', async () => {
    const user = userEvent.setup();
    const props = placementProps(['PLACEMENT_CONFIRMED', 'BACK'], true);
    renderUi(<WorkflowPanel {...props} />, locale);
    expect(screen.queryByTestId('placement-blocked')).toBeNull();
    const proceed = screen.getByRole('button', { name: t('workflow.action.placement_confirmed') }) as HTMLButtonElement;
    expect(proceed.disabled).toBe(false);
    await user.click(proceed);
    expect(props.sendCommand).toHaveBeenCalledExactlyOnceWith('PLACEMENT_CONFIRMED', 7);
  });

  it('lets the operator listen to the geophones at the ranging result without recording', async () => {
    const user = userEvent.setup();
    const props = panelProps('RANGING_RESULT', ['CONFIRM', 'BACK']);
    props.status!.acquisition.running = false;
    const onAcquisition = vi.fn<NonNullable<Props['onAcquisition']>>().mockResolvedValue(undefined);
    renderUi(<WorkflowPanel {...props} onAcquisition={onAcquisition} />, locale);
    expect(screen.queryByTestId('live-seismic')).toBeNull();
    await user.click(screen.getByRole('button', { name: t('workflow.live.show') }));
    expect(onAcquisition).toHaveBeenCalledExactlyOnceWith(true);
    // No workflow command is involved: listening never starts a recording.
    expect(props.sendCommand).not.toHaveBeenCalled();
  });

  it('shows the live signal panel once the preview runs and stops the preview when hidden', async () => {
    const user = userEvent.setup();
    const props = panelProps('RANGING_RESULT', ['CONFIRM', 'BACK']);
    props.status!.acquisition.running = true;
    const onAcquisition = vi.fn<NonNullable<Props['onAcquisition']>>().mockResolvedValue(undefined);
    renderUi(<WorkflowPanel {...props} onAcquisition={onAcquisition} />, locale);
    await user.click(screen.getByRole('button', { name: t('workflow.live.show') }));
    expect(onAcquisition).not.toHaveBeenCalled();
    expect(await screen.findByTestId('live-seismic')).toBeTruthy();
    await user.click(screen.getByRole('button', { name: t('workflow.live.hide') }));
    expect(onAcquisition).toHaveBeenCalledExactlyOnceWith(false);
    expect(screen.queryByTestId('live-seismic')).toBeNull();
  });

  // The live signals come only after the BLE Channel Sounding positions: never during calibration, placing or ranging.
  it.each(['SENSOR_CHECK', 'CALIB_PREP', 'CALIBRATING', 'CALIB_RESULT', 'PLACEMENT', 'RANGING', 'RECORD_READY'] as const)('offers no live signals at %s', (step) => {
    const props = panelProps(step, ['BACK']);
    renderUi(<WorkflowPanel {...props} onAcquisition={vi.fn()} />, locale);
    expect(screen.queryByRole('button', { name: t('workflow.live.show') })).toBeNull();
  });

  it('tells the operator after the calibration that placing the geophones comes next, not the live view', () => {
    const props = panelProps('CALIB_RESULT', ['PLACE_SENSORS', 'BACK']);
    renderUi(<WorkflowPanel {...props} onAcquisition={vi.fn()} />, locale);
    expect(screen.getByText(t('workflow.calib_result.next'))).toBeTruthy();
    expect(screen.queryByRole('button', { name: t('workflow.live.show') })).toBeNull();
  });

  it('shows no next-step hint when the calibration must be repeated', () => {
    const props = panelProps('CALIB_RESULT', ['RETRY', 'BACK']);
    renderUi(<WorkflowPanel {...props} />, locale);
    expect(screen.queryByText(t('workflow.calib_result.next'))).toBeNull();
  });

  // STOPPED loads the material settings of the object; the other steps in these tests make no HTTP request.
  function modelStoppedSettings() {
    respondJson('/api/localization/runs?job=settings&object_id=OBJ_0001', {
      ok: true, settings: null, presets: [],
      standard_processing: {
        timing_uncertainty_us: 2, noise_window_samples: 100, sta_window_samples: 20,
        lta_window_samples: 200, sta_lta_trigger_ratio: 3, refine_lookback_samples: 20,
        refine_lookahead_samples: 20, refine_amplitude_fraction: 0.5,
        snr_signal_window_samples: 40, minimum_snr: 2, semblance_window_half_span_samples: 8,
        s_pick_enabled: false, time_budget_s: 60,
      },
    });
  }

  it('leads from STOPPED to the recordings of this object', async () => {
    const user = userEvent.setup();
    modelStoppedSettings();
    const props = panelProps('STOPPED', ['BACK']);
    Object.assign(props.status!.workflow!, { object_id: 'OBJ_0001', session_id: 'S000002' });
    const onOpenRecordings = vi.fn<NonNullable<Props['onOpenRecordings']>>();
    renderUi(<WorkflowPanel {...props} onOpenRecordings={onOpenRecordings} />, locale);
    await user.click(await screen.findByRole('button', { name: t('workflow.open_recordings') }));
    expect(onOpenRecordings).toHaveBeenCalledExactlyOnceWith('OBJ_0001', 'S000002');
  });

  it.each(['PLACEMENT', 'RECORDING', 'CALCULATING'] as const)('does not offer the object recordings at %s', (step) => {
    const props = panelProps(step, ['BACK']);
    Object.assign(props.status!.workflow!, { object_id: 'OBJ_0001', session_id: 'S000002' });
    renderUi(<WorkflowPanel {...props} onOpenRecordings={vi.fn()} />, locale);
    expect(screen.queryByRole('button', { name: t('workflow.open_recordings') })).toBeNull();
  });

  it('offers the object recordings only when the app can open them', () => {
    modelStoppedSettings();
    const props = panelProps('STOPPED', ['BACK']);
    Object.assign(props.status!.workflow!, { object_id: 'OBJ_0001', session_id: 'S000002' });
    renderUi(<WorkflowPanel {...props} />, locale);
    expect(screen.queryByRole('button', { name: t('workflow.open_recordings') })).toBeNull();
  });

  it('releases the preview when the workflow moves past the live steps', async () => {
    const user = userEvent.setup();
    const props = panelProps('RANGING_RESULT', ['CONFIRM', 'BACK']);
    props.status!.acquisition.running = false;
    const onAcquisition = vi.fn<NonNullable<Props['onAcquisition']>>().mockResolvedValue(undefined);
    const view = renderUi(<WorkflowPanel {...props} onAcquisition={onAcquisition} />, locale);
    await user.click(screen.getByRole('button', { name: t('workflow.live.show') }));
    expect(onAcquisition).toHaveBeenCalledExactlyOnceWith(true);
    const next = panelProps('SCAN_CHOICE', ['SCAN_START', 'SCAN_SKIP', 'BACK']);
    view.rerender(<WorkflowPanel {...next} onAcquisition={onAcquisition} />);
    await waitFor(() => expect(onAcquisition).toHaveBeenLastCalledWith(false));
    expect(onAcquisition).toHaveBeenCalledTimes(2);
  });

  it('releases the preview when the panel is left while listening', async () => {
    const user = userEvent.setup();
    const props = panelProps('RANGING_RESULT', ['CONFIRM', 'BACK']);
    props.status!.acquisition.running = false;
    const onAcquisition = vi.fn<NonNullable<Props['onAcquisition']>>().mockResolvedValue(undefined);
    const view = renderUi(<WorkflowPanel {...props} onAcquisition={onAcquisition} />, locale);
    await user.click(screen.getByRole('button', { name: t('workflow.live.show') }));
    expect(onAcquisition).toHaveBeenCalledExactlyOnceWith(true);
    view.unmount();
    expect(onAcquisition).toHaveBeenLastCalledWith(false);
    expect(onAcquisition).toHaveBeenCalledTimes(2);
  });

  it('does not touch the acquisition when the preview was never shown', async () => {
    const props = panelProps('RANGING_RESULT', ['CONFIRM', 'BACK']);
    const onAcquisition = vi.fn<NonNullable<Props['onAcquisition']>>().mockResolvedValue(undefined);
    const view = renderUi(<WorkflowPanel {...props} onAcquisition={onAcquisition} />, locale);
    view.rerender(<WorkflowPanel {...panelProps('SCAN_CHOICE', ['SCAN_SKIP', 'BACK'])} onAcquisition={onAcquisition} />);
    view.unmount();
    expect(onAcquisition).not.toHaveBeenCalled();
  });

  it.each([0, 1, 2])('keeps permitted initialization reachable with %i remotes by keyboard', async (count) => {
    const user = userEvent.setup();
    const props = panelProps('SENSOR_CHECK', ['GO_CALIBRATION', 'BACK']);
    props.status!.pods = props.status!.pods.slice(0, count);
    props.status!.workflow!.assigned_remotes = count;
    props.status!.workflow!.sensor_sufficiency = 'INSUFFICIENT';
    renderUi(<WorkflowPanel {...props} />, locale);
    const start = screen.getByRole('button', { name: t('workflow.action.go_calibration') });
    await user.tab();
    expect(document.activeElement).toBe(start);
    expect((start as HTMLButtonElement).disabled).toBe(false);
    await user.keyboard('{Enter}');
    expect(props.sendCommand).toHaveBeenCalledExactlyOnceWith('GO_CALIBRATION', 7);
  });

  it('blocks duplicate submission while pending, then shows rejection and restores controls', async () => {
    const user = userEvent.setup();
    const props = panelProps('CALIB_PREP', ['SENSORS_PLACED', 'BACK']);
    let finish!: (result: WorkflowCommandResult) => void;
    vi.mocked(props.sendCommand).mockImplementation(() => new Promise((resolve) => { finish = resolve; }));
    renderUi(<WorkflowPanel {...props} />, locale);
    const submit = screen.getByRole('button', { name: t('workflow.action.sensors_placed') });
    await user.dblClick(submit);
    expect(props.sendCommand).toHaveBeenCalledExactlyOnceWith('SENSORS_PLACED', 7);
    for (const button of screen.getAllByRole('button')) expect((button as HTMLButtonElement).disabled).toBe(true);
    expect(submit.closest('[aria-busy]')?.getAttribute('aria-busy')).toBe('true');
    await act(async () => { finish({ ok: false, reason: 'error', message: 'fixture calibration rejected' }); });
    expect(screen.getByRole('alert').textContent).toContain('fixture calibration rejected');
    expect((submit as HTMLButtonElement).disabled).toBe(false);
    expect(props.onWorkflowUpdate).not.toHaveBeenCalled();
  });

  // The plan holds one execution per ordered pair, so the same three finished windows are a
  // smaller share of a bigger network: 3 of 6, 3 of 12, 3 of 20.
  it.each([[3, 6, '50'], [4, 12, '25'], [5, 20, '15']] as const)(
    'shows radio ranging progress as a share of all %i-node measurements', (nodes, total, percent) => {
      expect(total).toBe(nodes * (nodes - 1));
      const props = panelProps('RANGING', ['BACK']);
      (props.status as unknown as { ranging: unknown }).ranging = {
        available: true, progress: { pairs_done: 3, pairs_total: total },
      };
      renderUi(<WorkflowPanel {...props} />, locale);
      const text = document.body.textContent ?? '';
      expect(text).toContain(`3 / ${total} (${formatPercent(locale, 3 / total)})`);
      expect(text).toMatch(new RegExp(`\(${percent}\s?%\)`));
      expect(screen.getByRole('progressbar', { name: t('workflow.ranging.pairs') }).getAttribute('value'))
        .toBe(String((3 / total) * 100));
    });

  it('requires a second delete confirmation and Back cancels the armed action', async () => {
    const user = userEvent.setup();
    const props = panelProps('STOPPED', ['DELETE_RECORDING', 'BACK']);
    respondJson('/api/localization/runs?job=settings&object_id=OBJ_0001', {
      ok: true, settings: null, presets: [],
      standard_processing: {
        timing_uncertainty_us: 2, noise_window_samples: 100, sta_window_samples: 20,
        lta_window_samples: 200, sta_lta_trigger_ratio: 3, refine_lookback_samples: 20,
        refine_lookahead_samples: 20, refine_amplitude_fraction: 0.5,
        snr_signal_window_samples: 40, minimum_snr: 2, semblance_window_half_span_samples: 8,
        s_pick_enabled: false, time_budget_s: 60,
      },
    });
    renderUi(<WorkflowPanel {...props} />, locale);
    await user.click(screen.getByRole('button', { name: t('workflow.action.delete_recording') }));
    expect(screen.getByRole('alert').textContent).toBe(t('workflow.delete_confirm.prompt'));
    expect(props.sendCommand).not.toHaveBeenCalled();
    await user.click(screen.getByRole('button', { name: t('workflow.action.back') }));
    await waitFor(() => expect(screen.queryByRole('alert')).toBeNull());
    expect(props.sendCommand).toHaveBeenCalledExactlyOnceWith('BACK', 7);
    await user.click(screen.getByRole('button', { name: t('workflow.action.delete_recording') }));
    await user.click(screen.getByRole('button', { name: t('workflow.delete_confirm.button') }));
    expect(props.sendCommand).toHaveBeenNthCalledWith(2, 'DELETE_RECORDING', 7);
    expect(props.sendCommand).toHaveBeenCalledTimes(2);
  });

  it('refreshes stale state instead of reporting command success', async () => {
    const user = userEvent.setup();
    const props = panelProps('SENSOR_CHECK', ['GO_CALIBRATION']);
    vi.mocked(props.sendCommand).mockResolvedValue({ ok: false, reason: 'stale' });
    renderUi(<WorkflowPanel {...props} />, locale);
    await user.click(screen.getByRole('button', { name: t('workflow.action.go_calibration') }));
    expect(props.onRefreshStatus).toHaveBeenCalledTimes(1);
    expect(props.onWorkflowUpdate).not.toHaveBeenCalled();
    expect(screen.getByRole('status').textContent).toBe(t('workflow.state_changed'));
  });

  it('offers only actions allowed by the current device state', async () => {
    const user = userEvent.setup();
    const props = panelProps('SENSOR_CHECK', ['BACK']);
    renderUi(<WorkflowPanel {...props} />, locale);
    expect(screen.queryByRole('button', { name: t('workflow.action.go_calibration') })).toBeNull();
    await user.tab();
    expect(document.activeElement).toBe(screen.getByRole('button', { name: t('workflow.action.back') }));
    await user.keyboard(' ');
    expect(props.sendCommand).toHaveBeenCalledExactlyOnceWith('BACK', 7);
  });
});
