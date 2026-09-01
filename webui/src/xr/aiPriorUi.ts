import type { XrRuntimePhase } from './webxrRuntime';
import type { MonocularDepthProviderStatus } from './monocularDepthPrior';

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

function percent(value: number): string {
  return `${Math.round(Math.max(0, Math.min(1, value)) * 100)}%`;
}

/** Pure display policy: live AR is staging-only; inference/calibration happens after Finish. */
export function describeAiPrior(input: AiPriorUiInput): AiPriorUiState {
  if (!input.enabled) {
    return { shortStatus: 'OFF', calibrationLabel: '--', explanation: 'Optional AI is disabled.', good: false };
  }
  if (input.providerStatus === 'unavailable') {
    return { shortStatus: 'UNAVAILABLE', calibrationLabel: '--', explanation: 'WebGPU AI is unavailable; physical ARCore remains authoritative.', good: false };
  }
  if (input.phase === 'running' || input.phase === 'starting' || input.phase === 'stopping') {
    return {
      shortStatus: input.hasStagedPostScanFrame ? 'ARMED · POST-SCAN' : 'ARMING · POST-SCAN',
      calibrationLabel: input.hasStagedPostScanFrame ? 'PENDING AFTER FINISH' : 'WAITING FOR RGB+DEPTH',
      explanation: 'No ONNX inference runs in the live XR loop. The latest RGB frame and physical anchors are only staged for Finish.',
      good: input.hasStagedPostScanFrame,
    };
  }
  if (input.busy || input.providerStatus === 'loading') {
    return { shortStatus: 'RUNNING', calibrationLabel: 'CALIBRATING…', explanation: 'ONNX Depth Anything is running after the XR session.', good: false };
  }
  if (input.calibrationConfidence > 0) {
    const model = input.calibrationModel ? ` · ${input.calibrationModel}` : '';
    return {
      shortStatus: 'READY',
      calibrationLabel: `${percent(input.calibrationConfidence)}${model}`,
      explanation: 'AI passed physical-anchor calibration and may veto gross TSDF surface outliers only.',
      good: true,
    };
  }
  if (input.error !== null || input.providerStatus === 'error') {
    return { shortStatus: 'NOT APPLIED', calibrationLabel: 'CALIBRATION FAILED', explanation: input.error ?? 'AI calibration failed open.', good: false };
  }
  if (input.phase === 'ended' && !input.hasStagedPostScanFrame) {
    return {
      shortStatus: 'NOT APPLIED',
      calibrationLabel: 'NO STAGED RGB+DEPTH',
      explanation: 'The scan ended without enough pose-aligned physical RGB+depth anchors to run the optional prior.',
      good: false,
    };
  }
  return {
    shortStatus: 'ARMED · POST-SCAN',
    calibrationLabel: 'PENDING AFTER FINISH',
    explanation: 'AI is enabled but has not run yet.',
    good: false,
  };
}
