import type { RecordingGateStatus } from '../../domain/device';
import type { I18nKey } from '../../i18n';

type Translator = (key: I18nKey, params?: Readonly<Record<string, string | number>>) => string;

const REASON_KEY: Record<string, I18nKey> = {
  BLOCKED_INSUFFICIENT_REMOTES: 'recording_gate.reason.insufficient_remotes',
  BLOCKED_NOT_LEVEL: 'recording_gate.reason.not_level',
  STORAGE_UNAVAILABLE: 'recording_gate.reason.storage_unavailable',
};

export function recordingGateReason(gate: RecordingGateStatus, t: Translator): string | null {
  const reasonKey = gate.reason === undefined ? undefined : REASON_KEY[gate.reason];
  if (reasonKey !== undefined) return t(reasonKey);
  const decisionKey = gate.decision === undefined ? undefined : REASON_KEY[gate.decision];
  return decisionKey === undefined ? gate.decision ?? gate.reason ?? null : t(decisionKey);
}

export function recordingGateWarning(gate: RecordingGateStatus, t: Translator): string | null {
  if (gate.decision === 'ALLOWED_PLANAR_2D') return t('recording_gate.warning.planar');
  return gate.warning ?? null;
}
