import type {
  HardwareModuleId,
  HardwareModuleState,
  HardwareReasonCode,
} from '../../domain/device';

export type NormalizedHardwareModule = {
  state: HardwareModuleState | 'unrecognized';
  reasonCode: HardwareReasonCode;
  rawReason: string;
} & (
  | { id: HardwareModuleId; knownId: true }
  | { id: string; knownId: false }
);

const HARDWARE_MODULE_IDS: readonly HardwareModuleId[] = [
  'sd_card',
  'ad7606',
  'sensor_mux',
  'main_xiao',
  'barometer',
  'power_monitor',
];

const HARDWARE_MODULE_STATES: readonly HardwareModuleState[] = [
  'unknown',
  'present',
  'absent',
  'fault',
  'degraded',
];

/* Keep this table in sync with s_reason_codes in components/hw_inventory/hw_inventory_reason.c. */
export const HARDWARE_REASON_LEGACY_MAP: ReadonlyArray<readonly [string, HardwareReasonCode]> = [
  ['ready', 'ready'],
  ['no response', 'no_response'],
  ['no filesystem', 'no_filesystem'],
  ['mount failed', 'mount_failed'],
  ['read probe failed', 'read_probe_failed'],
  ['topology probe ok', 'topology_probe_ok'],
  ['topology probe fault', 'topology_probe_fault'],
  ['no upstream response', 'no_upstream_response'],
  ['sensor invalid', 'sensor_invalid'],
  ['first sample ok', 'first_sample_ok'],
  ['no valid sample', 'no_valid_sample'],
  ['busy responds', 'busy_responds'],
  ['no BUSY response', 'no_busy_response'],
  ['presence probe failed', 'presence_probe_failed'],
  ['blocked: 5V rail low', 'blocked_5v_rail_low'],
  ['init failed', 'init_failed'],
  ['start failed', 'start_failed'],
  ['sensor ok', 'sensor_ok'],
  ['upstream target assigned', 'upstream_target_assigned'],
  ['upstream topology fault', 'upstream_topology_fault'],
];

function isMember<T extends string>(values: readonly T[], value: unknown): value is T {
  return typeof value === 'string' && values.some((candidate) => candidate === value);
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null;
}

function normalizeHardwareState(value: unknown): HardwareModuleState | 'unrecognized' {
  return isMember(HARDWARE_MODULE_STATES, value) ? value : 'unrecognized';
}

function isHardwareReasonCode(value: unknown): value is HardwareReasonCode {
  return value === 'none' || value === 'unrecognized' || (
    typeof value === 'string' && HARDWARE_REASON_LEGACY_MAP.some(([, code]) => code === value)
  );
}

export function reasonCodeFromLegacyReason(reason: string | null | undefined): HardwareReasonCode {
  const normalizedReason = reason?.trim() ?? '';
  if (normalizedReason.length === 0) return 'none';

  for (const [legacyReason, code] of HARDWARE_REASON_LEGACY_MAP) {
    if (code === 'start_failed' ? normalizedReason.startsWith(legacyReason) : normalizedReason === legacyReason) {
      return code;
    }
  }

  return 'unrecognized';
}

/** Normalize untrusted hardware inventory fields without losing useful diagnostics. */
export function normalizeHardwareModule(raw: unknown): NormalizedHardwareModule {
  const module: Record<string, unknown> = isRecord(raw) ? raw : {};
  const rawId = typeof module.id === 'string' ? module.id : '';
  const rawState = module.state;
  const state: HardwareModuleState | 'unrecognized' = normalizeHardwareState(rawState);
  const rawReason = typeof module.reason === 'string' ? module.reason.trim() : '';
  const rawReasonCode = module.reason_code;
  const reasonCode: HardwareReasonCode = rawReasonCode === undefined
    ? reasonCodeFromLegacyReason(rawReason)
    : isHardwareReasonCode(rawReasonCode)
      ? rawReasonCode
      : 'unrecognized';

  const normalized: Pick<NormalizedHardwareModule, 'state' | 'reasonCode' | 'rawReason'> = {
    state,
    reasonCode,
    rawReason,
  };
  if (isMember(HARDWARE_MODULE_IDS, rawId)) {
    return { ...normalized, id: rawId, knownId: true };
  }
  return { ...normalized, id: rawId, knownId: false };
}
