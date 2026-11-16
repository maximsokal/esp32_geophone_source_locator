import { screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import PhoneCalculation from '../../src/features/workflow/PhoneCalculation';
import { translate } from '../../src/i18n';
import { PhoneCalculationError, type PhoneCalculationOutcome } from '../../src/wasm/phoneCalculation';
import { renderUi } from './setup';

const calculate = vi.hoisted(() => vi.fn());
vi.mock('../../src/wasm/phoneCalculation', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../src/wasm/phoneCalculation')>()),
  calculateOnPhone: calculate,
}));

function outcome(overrides: Partial<PhoneCalculationOutcome['result']> = {}): PhoneCalculationOutcome {
  return {
    frames: 26000,
    sampleRateHz: 4000,
    uploaded: { runs: 3, picks: true },
    result: {
      status: 0,
      picks: {},
      runs: [
        { method_name: 'semblance', solved: false },
        { method_name: 'tdoa_gauss_newton', solved: true, uncertainty_too_high: false, source_position: { x_m: 0.25, y_m: 0.4, z_m: 0.3 } },
        { method_name: 'bayesian', solved: true, uncertainty_too_high: true, source_position: { x_m: 9, y_m: 9, z_m: 9 } },
      ],
      channelHealth: { flatChannelMask: 0, identicalPairMask: 0 },
      ...overrides,
    },
  };
}

describe.each(['en', 'uk'] as const)('calculate on the phone %s', (locale) => {
  const t = (key: Parameters<typeof translate>[1], values?: Record<string, string | number>) => translate(locale, key, values);
  beforeEach(() => { calculate.mockReset(); });
  const runner = async () => async () => { throw new Error('the page worker is replaced in this test'); };

  it('does nothing until a finished session is known', () => {
    renderUi(<PhoneCalculation objectId={null} sessionId={null} loadRunner={runner as never} />, locale);
    expect((screen.getByRole('button', { name: t('workflow.phone.action') }) as HTMLButtonElement).disabled).toBe(true);
    expect(calculate).not.toHaveBeenCalled();
  });

  it('waits for valid calculation settings, like the calculation on the device', () => {
    renderUi(<PhoneCalculation objectId="OBJ_0001" sessionId="S000001" settingsReady={false} loadRunner={runner as never} />, locale);
    expect((screen.getByRole('button', { name: t('workflow.phone.action') }) as HTMLButtonElement).disabled).toBe(true);
  });

  it('shows the stages, then the source and how many methods found it', async () => {
    calculate.mockImplementation(async (_o, _s, _r, options) => {
      options.onStage('download');
      options.onDownloadProgress(500, 1000);
      options.onStage('upload');
      return outcome();
    });
    const user = userEvent.setup();
    renderUi(<PhoneCalculation objectId="OBJ_0001" sessionId="S000001" loadRunner={runner as never} />, locale);
    await user.click(screen.getByRole('button', { name: t('workflow.phone.action') }));
    const done = await screen.findByTestId('phone-calculation-done');
    expect(calculate).toHaveBeenCalledWith('OBJ_0001', 'S000001', expect.any(Function), expect.anything());
    // 2 of 3 methods solved; the source is the first solved one that is not too uncertain.
    expect(done.textContent).toContain('2');
    expect(done.textContent).toContain('tdoa_gauss_newton');
    expect(done.textContent).not.toContain('bayesian');
    expect(done.textContent).toMatch(/0[.,]25/);
    expect(screen.queryByTestId('phone-calculation-failed')).toBeNull();
  });

  it('names channels that were not independent sensors, like the device screen does', async () => {
    calculate.mockResolvedValue(outcome({ channelHealth: { flatChannelMask: 0b1001, identicalPairMask: 1 << 3 } }));
    const user = userEvent.setup();
    renderUi(<PhoneCalculation objectId="OBJ_0001" sessionId="S000001" loadRunner={runner as never} />, locale);
    await user.click(screen.getByRole('button', { name: t('workflow.phone.action') }));
    const done = await screen.findByTestId('phone-calculation-done');
    expect(done.textContent).toContain('G2=G3');
    expect(done.textContent).toContain('G1 G4');
  });

  it('says which step failed and offers to try again', async () => {
    calculate.mockRejectedValueOnce(new PhoneCalculationError('download', 'the file holds 5 frames, the session 6'));
    const user = userEvent.setup();
    renderUi(<PhoneCalculation objectId="OBJ_0001" sessionId="S000001" loadRunner={runner as never} />, locale);
    await user.click(screen.getByRole('button', { name: t('workflow.phone.action') }));
    const failed = await screen.findByTestId('phone-calculation-failed');
    expect(failed.textContent).toContain(t('workflow.phone.stage.download'));
    expect(failed.textContent).toContain('the file holds 5 frames');
    expect(screen.queryByTestId('phone-calculation-done')).toBeNull();
    await waitFor(() => expect((screen.getByRole('button', { name: t('workflow.phone.action') }) as HTMLButtonElement).disabled).toBe(false));
  });
});
