import { NRF_LOG_EVENTS } from './nrfLogEvents.gen';
import type { I18nKey } from '../../i18n';

type ArgumentSpec =
  | { kind: 'number' | 'hex' | 'signed'; labelKey: I18nKey }
  | { kind: 'addresses'; labelKey: I18nKey; base: number }
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
const NRF_UPSTREAM_MESSAGE_SENSOR_SCAN = 0x0F;
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
  NRF_UPSTREAM_MESSAGE_SENSOR_SCAN,
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

// Mirrors nrf_logger_led_source_t / nrf_logger_led_severity_t in nRF54L15/include/nrf_logger_led.h.
const loggerLatchSources = {
  0: key('logger_source.bmi160_fault'),
  1: key('logger_source.bme280_fault'),
  2: key('logger_source.log_ring_loss'),
  3: key('logger_source.twis_protocol'),
  4: key('logger_source.cs_ranging'),
  5: key('logger_source.watchdog_participant_stale'),
  6: key('logger_source.init_boot_fatal'),
  7: key('logger_source.role_lost'),
  9: key('logger_source.invalid_input'),
} as const;

const loggerLatchSeverities = {
  1: key('logger_severity.warning'),
  2: key('logger_severity.error'),
  3: key('logger_severity.fatal'),
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
  CS_PAIR_CONTEXT: [number(key('arg0')), hex(key('arg1'))],
  CS_PAIR_PHASE: [hex(key('attempt_id')), number(key('arg1'))],
  CS_PAIR_COMPLETE: [hex(key('attempt_id')), hex(key('arg1'))],
  CS_RF_SIGNAL: [hex(key('attempt_id')), hex(key('arg1'))],
  CS_RF_QUALITY: [hex(key('attempt_id')), hex(key('arg1'))],
  CS_RF_STATUS: [hex(key('attempt_id')), hex(key('arg1'))],
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
  LOGGER_LATCH: [
    enumeration(key('logger_source'), loggerLatchSources),
    enumeration(key('logger_severity'), loggerLatchSeverities),
  ],
  SENSOR_SCAN_WORD0: [hex(key('request_id')), { kind: 'addresses', labelKey: key('scan_addresses'), base: 0 }],
  SENSOR_SCAN_WORD1: [hex(key('request_id')), { kind: 'addresses', labelKey: key('scan_addresses'), base: 32 }],
  SENSOR_SCAN_WORD2: [hex(key('request_id')), { kind: 'addresses', labelKey: key('scan_addresses'), base: 64 }],
  SENSOR_SCAN_WORD3: [hex(key('request_id')), { kind: 'addresses', labelKey: key('scan_addresses'), base: 96 }],
  SENSOR_SCAN_DONE: [hex(key('request_id')), { kind: 'signed', labelKey: key('scan_result') }],
  // arg1 is unpacked by formatBusFault(); the spec only names the field.
  SENSOR_BUS_FAULT: [
    enumeration(key('sensor'), { 1: key('sensor.bmi160'), 2: key('sensor.bme280') }),
    number(key('bus_fault')),
  ],
} as const satisfies Record<EventName, readonly [ArgumentSpec, ArgumentSpec]>;

// nRF54L15/include/nrf_sensor_bus.h: nrf_sensor_bus_fault_pack() layout (bits 8-15 cause, 24-26 line levels).
const busFaultCauses = {
  1: key('bus_cause.timeout'),
  2: key('bus_cause.io'),
  3: key('bus_cause.other'),
} as const;

const busFaultLines = {
  released: key('bus_lines.released'),
  sdaLow: key('bus_lines.sda_low'),
  sclLow: key('bus_lines.scl_low'),
  bothLow: key('bus_lines.both_low'),
  unknown: key('bus_lines.unknown'),
} as const;

export const NRF_LOG_ARG_I18N_KEYS: readonly I18nKey[] = (() => {
  const keys = new Set<I18nKey>([key('arg0'), key('arg1'), key('raw_values'), key('unknown_value')]);
  keys.add(key('decoder_rejected'));
  for (const labelKey of ['bus_address', 'bus_cause', 'bus_errno', 'bus_lines', 'bus_failures']) keys.add(key(labelKey));
  for (const valueKey of [...Object.values(busFaultCauses), ...Object.values(busFaultLines)]) keys.add(valueKey);
  for (const valueKey of [...Object.values(logErrors), ...Object.values(upstreamResults)]) keys.add(valueKey);
  for (const name of ['local_rssi', 'peer_rssi', 'local_tx', 'peer_tx', 'local_tones', 'peer_tones',
    'good_channels', 'paths', 'config_issue', 'antenna_hw', 'noise_floor', 'snr', 'unavailable',
    'unknown', 'observed', 'none_observed', 'packet_errors', 'aborted', 'malformed', 'procedure',
    'role', 'initiator', 'reflector', 'proc_abort', 'sub_abort', 'no_sync', 'reference']) keys.add(key(`rf.${name}`));
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
  if (spec.kind === 'addresses') {
    const addresses = Array.from({ length: 32 }, (_, bit) => bit)
      .filter((bit) => (rawValue & (2 ** bit)) !== 0)
      .map((bit) => `0x${(spec.base + bit).toString(16).toUpperCase().padStart(2, '0')}`);
    return { labelKey: spec.labelKey, valueText: addresses.join(', ') || '—', raw: rawValue };
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

function formatBusFault(value: number): FormattedArgument[] {
  const raw = asUint32(value);
  const cause = (raw >>> 8) & 0xff;
  if (!(cause in busFaultCauses)) {
    return [{ labelKey: key('bus_fault'), valueKey: key('unknown_value'), raw }];
  }
  const address = raw & 0xff;
  const errno = (raw >>> 16) & 0xff;
  const lines = (raw >>> 24) & 0x07; // bit 0 SDA high, bit 1 SCL high, bit 2 levels valid
  const sdaHigh = (lines & 0x01) !== 0;
  const sclHigh = (lines & 0x02) !== 0;
  const linesKey = (lines & 0x04) === 0
    ? busFaultLines.unknown
    : sdaHigh && sclHigh ? busFaultLines.released
      : !sdaHigh && !sclHigh ? busFaultLines.bothLow
        : sdaHigh ? busFaultLines.sclLow : busFaultLines.sdaLow;
  return [
    { labelKey: key('bus_address'), valueText: `0x${address.toString(16).toUpperCase().padStart(2, '0')}`, raw: address },
    { labelKey: key('bus_cause'), valueKey: busFaultCauses[cause as keyof typeof busFaultCauses], raw: cause },
    { labelKey: key('bus_errno'), valueText: String(errno), raw: errno },
    { labelKey: key('bus_lines'), valueKey: linesKey, raw: lines },
    { labelKey: key('bus_failures'), valueText: String(raw >>> 27), raw: raw >>> 27 },
  ];
}

function rfNumber(label: string, value: number): FormattedArgument {
  return value === 127 ? { labelKey: key(`rf.${label}`), valueKey: key('rf.unavailable'), raw: value }
    : { labelKey: key(`rf.${label}`), valueText: String(value), raw: value };
}

function rfArgs(event: string, word: number): FormattedArgument[] {
  const dbm = (shift: number) => ((word >>> shift) & 0xff) << 24 >> 24;
  if (event === 'CS_RF_SIGNAL') {
    return [rfNumber('local_rssi', dbm(0)), rfNumber('peer_rssi', dbm(8)),
      rfNumber('local_tx', dbm(16)), rfNumber('peer_tx', dbm(24))];
  }
  const hardware: FormattedArgument = { labelKey: key('rf.antenna_hw'), valueKey: key('rf.unknown'), raw: 0 };
  if (event === 'CS_RF_QUALITY') {
    const flags = word >>> 25;
    return [rfNumber('local_tones', word & 127), rfNumber('peer_tones', (word >>> 7) & 127),
      rfNumber('good_channels', (word >>> 14) & 127), rfNumber('paths', (word >>> 21) & 15),
      { labelKey: key('rf.config_issue'), valueKey: key(flags & 1 ? 'rf.observed' : flags & 32 ? 'rf.none_observed' : 'rf.unknown'), raw: flags },
      { labelKey: key('rf.packet_errors'), valueText: `${Number((flags & 4) !== 0)}/${Number((flags & 8) !== 0)}`, raw: flags },
      rfNumber('aborted', Number((flags & 2) !== 0)), rfNumber('malformed', Number((flags & 16) !== 0)),
      { labelKey: key('rf.noise_floor'), valueKey: key('rf.unavailable'), raw: 127 },
      { labelKey: key('rf.snr'), valueKey: key('rf.unavailable'), raw: 127 }, hardware];
  }
  return [{ labelKey: key('rf.role'), valueKey: key(word >>> 31 ? 'rf.reflector' : 'rf.initiator'), raw: word >>> 31 },
    rfNumber('paths', word & 15), rfNumber('proc_abort', (word >>> 4) & 15),
    rfNumber('sub_abort', (word >>> 8) & 15), rfNumber('no_sync', Number(((word >>> 8) & 15) === 2)),
    rfNumber('reference', dbm(12)), rfNumber('local_tx', dbm(20)), hardware];
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
  if (eventName === 'SENSOR_BUS_FAULT') {
    return [formatArgument(specs[0], arg0), ...formatBusFault(arg1)];
  }
  if (eventName === 'CS_RF_SIGNAL' || eventName === 'CS_RF_QUALITY' || eventName === 'CS_RF_STATUS') {
    const context = eventName === 'CS_RF_STATUS' && (asUint32(arg1) >>> 31)
      ? formatArgument(number(key('rf.procedure')), arg0) : formatArgument(specs[0], arg0);
    return [context, ...rfArgs(eventName, asUint32(arg1))];
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
