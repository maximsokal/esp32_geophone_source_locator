import type { ComponentProps } from 'react';
import { act, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import WorkflowPanel from '../../src/features/workflow/WorkflowPanel';
import type { WorkflowCommandResult } from '../../src/api/deviceApi';
import { translate } from '../../src/i18n';
import { formatPercent } from '../../src/i18n/format';
import { readyStatus, renderUi, respondJson } from './setup';

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
