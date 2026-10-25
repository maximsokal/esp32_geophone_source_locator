import { NRF_LOG_EVENTS } from './nrfLogEvents.gen';
import type { I18nKey } from '../../i18n';

type ArgumentSpec =
  | { kind: 'number' | 'hex' | 'signed'; labelKey: I18nKey }
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

// Mirrors the firmware message type constants in these protocol headers.
const NRF_LOG_MESSAGE_STATUS_REQUEST = 0x07;
const NRF_LOG_MESSAGE_READ_REQUEST = 0x08;
const NRF_LOG_MESSAGE_STATUS = 0x82;
const NRF_LOG_MESSAGE_BLOCK = 0x83;
const NRF_UPSTREAM_MESSAGE_ASSIGN = 0x01;
const NRF_UPSTREAM_MESSAGE_SENSOR_REFRESH = 0x02;
const NRF_UPSTREAM_MESSAGE_HEIGHT_REFERENCE = 0x05;
const NRF_UPSTREAM_MESSAGE_IMU_CALIBRATE = 0x06;
const NRF_UPSTREAM_MESSAGE_STATUS = 0x80;
const NRF_CS_CONTROL_MESSAGE_EXECUTE = 0x03;
const NRF_CS_CONTROL_MESSAGE_STATUS_REQUEST = 0x04;
const NRF_CS_CONTROL_MESSAGE_RESULT = 0x81;

// nRF54L15/include/nrf_log_protocol.h: nrf_log_message_type_t.
const logMessageTypes = new Set([
  NRF_LOG_MESSAGE_STATUS_REQUEST,
  NRF_LOG_MESSAGE_READ_REQUEST,
  NRF_LOG_MESSAGE_STATUS,
  NRF_LOG_MESSAGE_BLOCK,
]);

// nRF54L15/include/nrf_upstream_protocol.h: nrf_upstream_message_type_t.
const upstreamMessageTypes = new Set([
  NRF_UPSTREAM_MESSAGE_ASSIGN,
  NRF_UPSTREAM_MESSAGE_SENSOR_REFRESH,
  NRF_UPSTREAM_MESSAGE_HEIGHT_REFERENCE,
  NRF_UPSTREAM_MESSAGE_IMU_CALIBRATE,
  NRF_UPSTREAM_MESSAGE_STATUS,
]);

// nRF54L15/include/nrf_cs_control_protocol.h: NRF_CS_CONTROL_MESSAGE_*.
const csControlMessageTypes = new Set([
  NRF_CS_CONTROL_MESSAGE_EXECUTE,
  NRF_CS_CONTROL_MESSAGE_STATUS_REQUEST,
  NRF_CS_CONTROL_MESSAGE_RESULT,
]);

// Values and names mirror nrf_upstream_result_t in nRF54L15/include/nrf_upstream_protocol.h.
const upstreamResults = {
  0: key('upstream_result.ok'), // NRF_UPSTREAM_RESULT_OK
  1: key('upstream_result.pending'), // NRF_UPSTREAM_RESULT_PENDING
  2: key('upstream_result.invalid_frame'), // NRF_UPSTREAM_RESULT_INVALID_FRAME
  3: key('upstream_result.bad_crc'), // NRF_UPSTREAM_RESULT_BAD_CRC
  4: key('upstream_result.unsupported_version'), // NRF_UPSTREAM_RESULT_UNSUPPORTED_VERSION
  5: key('upstream_result.unsupported_type'), // NRF_UPSTREAM_RESULT_UNSUPPORTED_TYPE
  6: key('upstream_result.request_id_invalid'), // NRF_UPSTREAM_RESULT_REQUEST_ID_INVALID
  7: key('upstream_result.hw_id_mismatch'), // NRF_UPSTREAM_RESULT_HW_ID_MISMATCH
  8: key('upstream_result.role_invalid'), // NRF_UPSTREAM_RESULT_ROLE_INVALID
  9: key('upstream_result.node_id_invalid'), // NRF_UPSTREAM_RESULT_NODE_ID_INVALID
  10: key('upstream_result.channel_invalid'), // NRF_UPSTREAM_RESULT_CHANNEL_INVALID
  11: key('upstream_result.role_conflict'), // NRF_UPSTREAM_RESULT_ROLE_CONFLICT
  12: key('upstream_result.request_conflict'), // NRF_UPSTREAM_RESULT_REQUEST_CONFLICT
  13: key('upstream_result.busy'), // NRF_UPSTREAM_RESULT_BUSY
  14: key('upstream_result.readdress_failed'), // NRF_UPSTREAM_RESULT_READDRESS_FAILED
  15: key('upstream_result.internal_error'), // NRF_UPSTREAM_RESULT_INTERNAL_ERROR
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
  2: key('participant.bt_cs_progress'),
} as const;

export const NRF_LOG_ARG_FORMATS = {
  BOOT: [hex(key('reset_cause_bits')), enumeration(key('boot_id_fallback'), bootIdFallback)],
  LOG_GENERATION_ROLLOVER: [hex(key('previous_boot_id')), hex(key('new_boot_id'))],
  TWIS_INVALID_FRAME: [hex(key('message_type')), number(key('decoder_error'))],
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
  FAULT_RECORD_WORD: [number(key('arg0')), hex(key('arg1'))],
  BOOT_FAILED: [number(key('arg0')), number(key('arg1'))],
  CS_EVENT_QUEUE_OVERFLOW: [number(key('arg0')), number(key('arg1'))],
  WATCHDOG_DEGRADED: [number(key('arg0')), number(key('arg1'))],
  WATCHDOG_FEED_FAILED: [number(key('arg0')), number(key('arg1'))],
  SENSOR_INIT_FAILED: [
    enumeration(key('sensor'), { 1: key('sensor.bmi160'), 2: key('sensor.bme280') }),
    { kind: 'signed', labelKey: key('init_result') },
  ],
} as const satisfies Record<EventName, readonly [ArgumentSpec, ArgumentSpec]>;

export const NRF_LOG_ARG_I18N_KEYS: readonly I18nKey[] = (() => {
  const keys = new Set<I18nKey>([key('arg0'), key('arg1'), key('raw_values'), key('unknown_value')]);
  keys.add(key('decoder_rejected'));
  for (const valueKey of [...Object.values(logErrors), ...Object.values(upstreamResults)]) keys.add(valueKey);
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
    valueText: spec.kind === 'hex' ? formatHex(rawValue) : String(spec.kind === 'signed' ? rawValue | 0 : rawValue),
    raw: rawValue,
  };
}

function formatDecoderError(messageType: number, result: number): FormattedArgument {
  const type = asUint32(messageType);
  if (logMessageTypes.has(type)) {
    return formatArgument(enumeration(key('decoder_error'), logErrors), result);
  }
  if (upstreamMessageTypes.has(type)) {
    return formatArgument(enumeration(key('decoder_error'), upstreamResults), result);
  }
  if (csControlMessageTypes.has(type)) {
    return { labelKey: key('decoder_error'), valueKey: key('decoder_rejected'), raw: asUint32(result) };
  }
  return formatArgument(number(key('decoder_error')), result);
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
  if (eventName === 'TWIS_INVALID_FRAME') {
    return [formatArgument(specs[0], arg0), formatDecoderError(arg0, arg1)];
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
