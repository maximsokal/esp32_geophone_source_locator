import { useState } from 'react';
import { useLocale, useT, type I18nKey } from '../../i18n';
import { formatNumber } from '../../i18n/format';
import { calculateOnPhone, PhoneCalculationError, type KernelRunner, type PhoneCalculationOutcome, type PhoneCalculationStage } from '../../wasm/phoneCalculation';
import { channelHealthLabels } from './workflowViewModel';

/**
 * "Calculate on this phone": the recording is downloaded, the firmware's own calculation runs here (WebAssembly) and only
 * the result goes back to the ESP32, which stays free for recording. Offered on the phone only; the device's own screen
 * has no such choice.
 */
const STAGE_KEYS: Record<PhoneCalculationStage, I18nKey> = {
  meta: 'workflow.phone.stage.meta',
  settings: 'workflow.phone.stage.settings',
  download: 'workflow.phone.stage.download',
  calculate: 'workflow.phone.stage.calculate',
  upload: 'workflow.phone.stage.upload',
};

type State =
  | { kind: 'idle' }
  | { kind: 'running'; stage: PhoneCalculationStage; progress: number | null }
  | { kind: 'done'; outcome: PhoneCalculationOutcome }
  | { kind: 'failed'; stage: PhoneCalculationStage; message: string };

interface Props {
  objectId: string | null;
  sessionId: string | null;
  /** The object's calculation settings are valid: until then the calculation cannot start, here or on the device. */
  settingsReady?: boolean;
  /** The worker in the page; tests hand in their own. */
  loadRunner?: () => Promise<KernelRunner>;
}

async function defaultRunner(): Promise<KernelRunner> {
  return (await import('../../wasm/kernelClient')).runKernelInWorker;
}

/** The first solved method that is not rejected for its uncertainty, in the order the firmware ranks them. */
function bestSource(outcome: PhoneCalculationOutcome): { x: number; y: number; z: number; method: string } | null {
  for (const run of outcome.result.runs as Array<Record<string, any>>) {
    if (run.solved === true && run.uncertainty_too_high !== true && run.source_position !== undefined) {
      return { x: run.source_position.x_m, y: run.source_position.y_m, z: run.source_position.z_m, method: String(run.method_name) };
    }
  }
  return null;
}

export default function PhoneCalculation({ objectId, sessionId, settingsReady = true, loadRunner = defaultRunner }: Props) {
  const t = useT();
  const { locale } = useLocale();
  const [state, setState] = useState<State>({ kind: 'idle' });
  const ready = objectId !== null && sessionId !== null && settingsReady && state.kind !== 'running';

  async function start() {
    if (objectId === null || sessionId === null) return;
    setState({ kind: 'running', stage: 'meta', progress: null });
    try {
      const runner = await loadRunner();
      const outcome = await calculateOnPhone(objectId, sessionId, runner, {
        onStage: (stage) => setState({ kind: 'running', stage, progress: null }),
        onDownloadProgress: (received, total) => setState({ kind: 'running', stage: 'download', progress: total > 0 ? received / total : null }),
      });
      setState({ kind: 'done', outcome });
    } catch (reason) {
      const stage = reason instanceof PhoneCalculationError ? reason.stage : 'calculate';
      setState({ kind: 'failed', stage, message: reason instanceof Error ? reason.message : String(reason) });
    }
  }

  const number = (value: number) => formatNumber(locale, value, { maximumFractionDigits: 2 });
  return (
    <section className="workflow-phone-calculation" aria-label={t('workflow.phone.title')}>
      <h3>{t('workflow.phone.title')}</h3>
      <p>{t('workflow.phone.hint')}</p>
      <button type="button" disabled={!ready} onClick={() => { void start(); }}>{t('workflow.phone.action')}</button>
      {state.kind === 'running' && (
        <p role="status" data-testid="phone-calculation-stage">
          {t(STAGE_KEYS[state.stage])}
          {state.progress !== null && ` ${formatNumber(locale, Math.round(state.progress * 100))}%`}
        </p>
      )}
      {state.kind === 'failed' && (
        <p role="alert" data-testid="phone-calculation-failed">
          {t('workflow.phone.failed', { stage: t(STAGE_KEYS[state.stage]), message: state.message })}
        </p>
      )}
      {state.kind === 'done' && (() => {
        const source = bestSource(state.outcome);
        const runs = state.outcome.result.runs as Array<Record<string, any>>;
        const solved = runs.filter((run) => run.solved === true).length;
        const health = channelHealthLabels({
          flat_channels: bitsToChannels(state.outcome.result.channelHealth.flatChannelMask),
          identical_pairs: pairsFromMask(state.outcome.result.channelHealth.identicalPairMask),
        });
        return (
          <div data-testid="phone-calculation-done">
            <p role="status">{t('workflow.phone.done', { solved: formatNumber(locale, solved), total: formatNumber(locale, runs.length) })}</p>
            <p>{source === null
              ? t('workflow.phone.no_source')
              : t('workflow.phone.source', { x: number(source.x), y: number(source.y), z: number(source.z), method: source.method })}</p>
            {health.identicalChannels && <p role="alert">{t('workflow.result.channels_identical', { channels: health.identicalChannels })}</p>}
            {health.silentChannels && <p role="alert">{t('workflow.result.channels_silent', { channels: health.silentChannels })}</p>}
          </div>
        );
      })()}
    </section>
  );
}

function bitsToChannels(mask: number): number[] {
  return [1, 2, 3, 4].filter((channel) => (mask & (1 << (channel - 1))) !== 0);
}

/** Pair numbering of localization_channel_health: 0:(1,2) 1:(1,3) 2:(1,4) 3:(2,3) 4:(2,4) 5:(3,4), channels counted from 1. */
const PAIRS: ReadonlyArray<readonly [number, number]> = [[1, 2], [1, 3], [1, 4], [2, 3], [2, 4], [3, 4]];
function pairsFromMask(mask: number): { a: number; b: number }[] {
  return PAIRS.flatMap(([a, b], index) => ((mask & (1 << index)) !== 0 ? [{ a, b }] : []));
}
