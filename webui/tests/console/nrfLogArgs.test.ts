import { describe, expect, it } from 'vitest';

import { NRF_LOG_EVENTS } from '../../src/features/nrflog/nrfLogEvents.gen';
import {
  formatEventArgs,
  formatEventArgsText,
  NRF_LOG_ARG_FORMATS,
  NRF_LOG_ARG_I18N_KEYS,
} from '../../src/features/nrflog/nrfLogArgs';
import { en } from '../../src/i18n/catalog.en.gen';
import { uk } from '../../src/i18n/catalog.uk.gen';
import type { I18nKey } from '../../src/i18n';

const english = (key: I18nKey): string => en[key];
const ukrainian = (key: I18nKey): string => uk[key];

describe('nRF log event arguments', () => {
  it.each([
    ['BOOT', 1, 0x12, 1, 'Reset cause bits (0 = unknown): 0x00000012, Boot ID fallback: Used (1)'],
    ['LOG_GENERATION_ROLLOVER', 2, 11, 12, 'Previous boot ID: 0x0000000B, New boot ID: 0x0000000C'],
    ['TWIS_INVALID_FRAME', 3, 0x07, 6, 'Message type: 0x00000007, Decoder error: Bad CRC (6)'],
    ['TWIS_INVALID_FRAME', 3, 0x08, 5, 'Message type: 0x00000008, Decoder error: Bad length (5)'],
    ['TWIS_INVALID_FRAME', 3, 0x82, 7, 'Message type: 0x00000082, Decoder error: Reserved fields nonzero (7)'],
    ['TWIS_INVALID_FRAME', 3, 0x83, 2, 'Message type: 0x00000083, Decoder error: Bad magic (2)'],
    ['TWIS_INVALID_FRAME', 3, 0x01, 2, 'Message type: 0x00000001, Decoder error: Invalid frame (2)'],
    ['TWIS_INVALID_FRAME', 3, 0x02, 3, 'Message type: 0x00000002, Decoder error: Bad CRC (3)'],
    ['TWIS_INVALID_FRAME', 3, 0x05, 13, 'Message type: 0x00000005, Decoder error: Busy (13)'],
    ['TWIS_INVALID_FRAME', 3, 0x06, 15, 'Message type: 0x00000006, Decoder error: Internal error (15)'],
    ['TWIS_INVALID_FRAME', 3, 0x80, 5, 'Message type: 0x00000080, Decoder error: Unsupported type (5)'],
    ['TWIS_INVALID_FRAME', 3, 0x03, 1, 'Message type: 0x00000003, Decoder error: Decoder rejected (1)'],
    ['TWIS_INVALID_FRAME', 3, 0x04, 1, 'Message type: 0x00000004, Decoder error: Decoder rejected (1)'],
    ['TWIS_INVALID_FRAME', 3, 0x81, 1, 'Message type: 0x00000081, Decoder error: Decoder rejected (1)'],
    ['TWIS_INVALID_FRAME', 3, 0xFF, 6, 'Message type: 0x000000FF, Decoder error: 6'],
    ['TWIS_UNKNOWN_TYPE', 4, 0x81, 9, 'Message type: 0x00000081, Frame length (bytes): 9'],
    ['TWIS_BUSY', 5, 5, 4, 'Message type: 0x00000005, Work queue capacity: 4'],
    ['ROLE_ASSIGNED', 6, 23, 2, 'Node ID: 0x00000017, Role: REMOTE (2)'],
    ['ROLE_LOST', 7, 23, 1, 'Node ID: 0x00000017, Role: MAIN (1)'],
    ['IMU_FAULT_SET', 8, 4, 12, 'Fault mask bit: 0x00000004, Snapshot sequence: 12'],
    ['IMU_FAULT_CLEARED', 9, 4, 13, 'Fault mask bit: 0x00000004, Snapshot sequence: 13'],
    ['ENV_FAULT_SET', 10, 2, 14, 'Fault mask bit: 0x00000002, Snapshot sequence: 14'],
    ['ENV_FAULT_CLEARED', 11, 2, 15, 'Fault mask bit: 0x00000002, Snapshot sequence: 15'],
    ['HEIGHT_REF_REJECTED', 12, 33, 2, 'Request ID: 0x00000021, Reference state: Rejected: moving (2)'],
    ['IMU_CAL_REJECTED', 13, 34, 3, 'Request ID: 0x00000022, Calibration state: Rejected: gravity (3)'],
    ['CS_EXECUTE_FAILED', 14, 35, 2, 'Attempt ID: 0x00000023, Transport/outcome error: Transport error (2)'],
    ['CS_TIMEOUT', 15, 36, 2500, 'Attempt ID: 0x00000024, Timeout (ms): 2500'],
    ['WATCHDOG_PARTICIPANT_STALE', 16, 1, 5000, 'Participant: System work queue (1), Allowed silence (ms): 5000'],
    ['LOG_TEST_EVENT', 17, 7, 100000, 'Test index: 7, Uptime (ms): 100000'],
    ['SENSOR_INIT_FAILED', 23, 1, 0xFFFFFFFB, 'Sensor: BMI160 (1), Driver initialization result: -5'],
    ['SENSOR_INIT_FAILED', 23, 2, 0xFFFFFFED, 'Sensor: BME280 (2), Driver initialization result: -19'],
  ] as const)('decodes %s (event %i)', (_name, eventId, arg0, arg1, expected) => {
    expect(formatEventArgsText(formatEventArgs(eventId, arg0, arg1), english)).toBe(expected);
  });

  it('uses the same table for event names and IDs', () => {
    expect(formatEventArgs('TWIS_INVALID_FRAME', 0x07, 6)).toEqual(formatEventArgs(3, 0x07, 6));
  });

  it('localizes decoder-rejected messages in Ukrainian and preserves the raw code', () => {
    expect(formatEventArgsText(formatEventArgs(3, 0x03, 1), ukrainian))
      .toBe('Тип повідомлення: 0x00000003, Помилка декодера: Декодер відхилив запит (1)');
  });

  it('renders uint32 boundaries as unsigned and IDs or masks as hexadecimal', () => {
    expect(formatEventArgs(1, 0, 0).map((arg) => arg.raw)).toEqual([0, 0]);
    expect(formatEventArgs(6, 0xFFFFFFFF, 0x80000000)).toEqual([
      { labelKey: 'nrf_log.arg.node_id', valueText: '0xFFFFFFFF', raw: 0xFFFFFFFF },
      { labelKey: 'nrf_log.arg.role', valueKey: 'nrf_log.arg.unknown_value', raw: 0x80000000 },
    ]);
    expect(formatEventArgs(17, 0x80000000, 0xFFFFFFFF).map((arg) => arg.valueText)).toEqual([
      '2147483648', '4294967295',
    ]);
  });

  it('shows unknown events and unknown enum values with localized fallback text and raw values', () => {
    expect(formatEventArgsText(formatEventArgs('FUTURE_EVENT', 1234, 0xFFFFFFFF), english))
      .toBe('arg0: Unknown value (1234), arg1: Unknown value (4294967295)');
    expect(formatEventArgsText(formatEventArgs(3, 0x01, 999), english))
      .toBe('Message type: 0x00000001, Decoder error: Unknown value (999)');
  });

  it('has argument formatting for every generated event and EN/UK text for every referenced key', () => {
    for (const event of Object.values(NRF_LOG_EVENTS)) {
      expect(NRF_LOG_ARG_FORMATS).toHaveProperty(event.name);
    }
    for (const key of NRF_LOG_ARG_I18N_KEYS) {
      expect(en[key], `${key} English`).toBeTruthy();
      expect(uk[key], `${key} Ukrainian`).toBeTruthy();
    }
  });
});
