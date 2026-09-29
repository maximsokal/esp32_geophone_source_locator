import { NRF_LOG_EVENTS } from './nrfLogEvents.gen';
import type { I18nKey } from '../../i18n';

type ArgumentSpec =
  | { kind: 'number' | 'hex'; labelKey: I18nKey }
  | { kind: 'enum'; labelKey: I18nKey; values: Readonly<Record<number, I18nKey>> };

type EventName = (typeof NRF_LOG_EVENTS)[keyof typeof NRF_LOG_EVENTS]['name'];
type FormattedArgument =
  | { labelKey: I18nKey; valueText: string; raw: number; valueKey?: never }
  | { labelKey: I18nKey; valueKey: I18nKey; raw: number; valueText?: never };

const key = (name: string): I18nKey => `nrf_log.arg.${name}` as I18nKey;
const number = (labelKey: I18nKey): ArgumentSpec => ({ kind: 'number', labelKey });
const hex = (labelKey: I18nKey): ArgumentSpec => ({ kind: 'hex', labelKey });
const enumeration = (labelKey: I18nKey, values: Readonly<Record<number, I18nKey>>): ArgumentSpec => ({ kind: 'enum', labelKey, values });

const bootIdFallback = {
  0: key('boot_id_fallback.no'),
  1: key('boot_id_fallback.yes'),
} as const;

const roles = {
  0: key('role.unassigned'),
  1: key('role.main'),
  2: key('role.remote'),
} as const;

const logErrors = {
  0: key('error.ok'),
  1: key('error.null_argument'),
  2: key('error.bad_magic'),
  3: key('error.bad_version'),
  4: key('error.bad_type'),
  5: key('error.bad_length'),
  6: key('error.bad_crc'),
  7: key('error.reserved_nonzero'),
  8: key('error.bad_flags'),
  9: key('error.bad_severity'),
  10: key('error.bad_subsystem'),
  11: key('error.count_overflow'),
  12: key('error.seq_mismatch'),
  13: key('error.uptime_regression'),
  14: key('error.bad_boot_id'),
  15: key('error.invariant_violation'),
  16: key('error.reentrant_loss'),
} as const;

const referenceStates = {
  0: key('reference.not_established'),
  1: key('reference.valid'),
  2: key('reference.rejected_moving'),
  3: key('reference.sensor_invalid'),
} as const;

const calibrationStates = {
  0: key('calibration.not_calibrated'),
  1: key('calibration.calibrated'),
  2: key('calibration.rejected_moving'),
  3: key('calibration.rejected_gravity'),
  4: key('calibration.sensor_invalid'),
} as const;

const csOutcomes = {
  0: key('cs_outcome.success'),
  1: key('cs_outcome.timeout'),
  2: key('cs_outcome.transport_error'),
  3: key('cs_outcome.outlier'),
} as const;

const watchdogParticipants = {
  0: key('participant.sensor_worker'),
  1: key('participant.system_workqueue'),
} as const;

export const NRF_LOG_ARG_FORMATS = {
  BOOT: [hex(key('reset_cause_bits')), enumeration(key('boot_id_fallback'), bootIdFallback)],
  LOG_GENERATION_ROLLOVER: [hex(key('previous_boot_id')), hex(key('new_boot_id'))],
  TWIS_INVALID_FRAME: [hex(key('message_type')), enumeration(key('decoder_error'), logErrors)],
  TWIS_UNKNOWN_TYPE: [hex(key('message_type')), number(key('frame_length'))],
  TWIS_BUSY: [hex(key('message_type')), number(key('work_queue_capacity'))],
  ROLE_ASSIGNED: [hex(key('node_id')), enumeration(key('role'), roles)],
  ROLE_LOST: [hex(key('node_id')), enumeration(key('role'), roles)],
  IMU_FAULT_SET: [hex(key('fault_mask_bit')), number(key('snapshot_sequence'))],
  IMU_FAULT_CLEARED: [hex(key('fault_mask_bit')), number(key('snapshot_sequence'))],
  ENV_FAULT_SET: [hex(key('fault_mask_bit')), number(key('snapshot_sequence'))],
  ENV_FAULT_CLEARED: [hex(key('fault_mask_bit')), number(key('snapshot_sequence'))],
  HEIGHT_REF_REJECTED: [hex(key('request_id')), enumeration(key('reference_state'), referenceStates)],
  IMU_CAL_REJECTED: [hex(key('request_id')), enumeration(key('calibration_state'), calibrationStates)],
  CS_EXECUTE_FAILED: [hex(key('attempt_id')), enumeration(key('transport_outcome_error'), csOutcomes)],
  CS_TIMEOUT: [hex(key('attempt_id')), number(key('timeout_ms'))],
  WATCHDOG_PARTICIPANT_STALE: [enumeration(key('participant'), watchdogParticipants), number(key('allowed_silence_ms'))],
  LOG_TEST_EVENT: [number(key('test_index')), number(key('uptime_ms'))],
} as const satisfies Record<EventName, readonly [ArgumentSpec, ArgumentSpec]>;

export const NRF_LOG_ARG_I18N_KEYS: readonly I18nKey[] = (() => {
  const keys = new Set<I18nKey>([key('arg0'), key('arg1'), key('raw_values'), key('unknown_value')]);
  for (const [first, second] of Object.values(NRF_LOG_ARG_FORMATS)) {
    for (const spec of [first, second]) {
      keys.add(spec.labelKey);
      if (spec.kind === 'enum') Object.values(spec.values).forEach((valueKey) => keys.add(valueKey));
    }
  }
  return [...keys];
})();

function asUint32(value: number): number {
  return value >>> 0;
}

function formatHex(value: number): string {
  return `0x${value.toString(16).toUpperCase().padStart(8, '0')}`;
}

function formatArgument(spec: ArgumentSpec, value: number): FormattedArgument {
  const rawValue = asUint32(value);
  if (spec.kind === 'enum') {
    return { labelKey: spec.labelKey, valueKey: spec.values[rawValue] ?? key('unknown_value'), raw: rawValue };
  }
  return {
    labelKey: spec.labelKey,
    valueText: spec.kind === 'hex' ? formatHex(rawValue) : String(rawValue),
    raw: rawValue,
  };
}

export function formatEventArgs(eventNameOrId: string | number, arg0: number, arg1: number): FormattedArgument[] {
  const eventName = typeof eventNameOrId === 'number'
    ? NRF_LOG_EVENTS[asUint32(eventNameOrId) as keyof typeof NRF_LOG_EVENTS]?.name
    : eventNameOrId;
  const specs = eventName === undefined
    ? undefined
    : NRF_LOG_ARG_FORMATS[eventName as EventName];
  if (specs === undefined) {
    return [
      { labelKey: key('arg0'), valueKey: key('unknown_value'), raw: asUint32(arg0) },
      { labelKey: key('arg1'), valueKey: key('unknown_value'), raw: asUint32(arg1) },
    ];
  }
  return [formatArgument(specs[0], arg0), formatArgument(specs[1], arg1)];
}

export function formatEventArgsText(
  args: readonly FormattedArgument[],
  translate: (key: I18nKey) => string,
): string {
  return args.map((arg) => {
    const value = arg.valueText ?? `${translate(arg.valueKey)} (${arg.raw})`;
    return `${translate(arg.labelKey)}: ${value}`;
  }).join(', ');
}
