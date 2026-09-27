import type { XrRuntimePhase } from './webxrRuntime';
import type { MonocularDepthProviderStatus } from './monocularDepthPrior';
import { translate, type I18nLocale } from '../i18n';
import { formatPercent } from '../i18n/format';

export interface AiPriorUiInput {
  enabled: boolean;
  phase: XrRuntimePhase;
  providerStatus: MonocularDepthProviderStatus;
  hasStagedPostScanFrame: boolean;
  busy: boolean;
  calibrationConfidence: number;
  calibrationModel: string | null;
  error: string | null;
}

export interface AiPriorUiState {
  shortStatus: string;
  calibrationLabel: string;
  explanation: string;
  good: boolean;
}

/** Pure display policy: live AR is staging-only; inference/calibration happens after Finish. */
export function describeAiPrior(input: AiPriorUiInput, locale: I18nLocale = 'en'): AiPriorUiState {
  if (!input.enabled) {
    return { shortStatus: 'OFF', calibrationLabel: '--', explanation: translate(locale, 'xr.ai.disabled'), good: false };
  }
  if (input.providerStatus === 'unavailable') {
    return { shortStatus: translate(locale, 'xr.ai.unavailable'), calibrationLabel: '--', explanation: translate(locale, 'xr.ai.webgpu_unavailable'), good: false };
  }
  if (input.phase === 'running' || input.phase === 'starting' || input.phase === 'stopping') {
    return {
      shortStatus: translate(locale, input.hasStagedPostScanFrame ? 'xr.ai.armed' : 'xr.ai.arming'),
      calibrationLabel: translate(locale, input.hasStagedPostScanFrame ? 'xr.ai.pending' : 'xr.ai.waiting_rgb_depth'),
      explanation: translate(locale, 'xr.ai.live_staging'),
      good: input.hasStagedPostScanFrame,
    };
  }
  if (input.busy || input.providerStatus === 'loading') {
    return { shortStatus: translate(locale, 'xr.ai.running'), calibrationLabel: translate(locale, 'xr.ai.calibrating'), explanation: translate(locale, 'xr.ai.post_scan_running'), good: false };
  }
  if (input.calibrationConfidence > 0) {
    const model = input.calibrationModel ? ` · ${input.calibrationModel}` : '';
    return {
      shortStatus: translate(locale, 'scan.ready'),
      calibrationLabel: `${formatPercent(locale, input.calibrationConfidence)}${model}`,
      explanation: translate(locale, 'xr.ai.calibrated'),
      good: true,
    };
  }
  if (input.error !== null || input.providerStatus === 'error') {
    return { shortStatus: translate(locale, 'xr.ai.not_applied'), calibrationLabel: translate(locale, 'xr.ai.calibration_failed'), explanation: input.error ?? translate(locale, 'xr.ai.failed_open'), good: false };
  }
  if (input.phase === 'ended' && !input.hasStagedPostScanFrame) {
    return {
      shortStatus: translate(locale, 'xr.ai.not_applied'),
      calibrationLabel: translate(locale, 'xr.ai.no_staged_rgb_depth'),
      explanation: translate(locale, 'xr.ai.no_staged_explanation'),
      good: false,
    };
  }
  return {
    shortStatus: translate(locale, 'xr.ai.armed'),
    calibrationLabel: translate(locale, 'xr.ai.pending'),
    explanation: translate(locale, 'xr.ai.not_run'),
    good: false,
  };
}
