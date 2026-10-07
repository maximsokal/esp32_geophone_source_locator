import { useEffect, useMemo, useRef, useState } from 'react';

import {
  ApiRequestOwnerRetiredError,
  ApiRequestPreemptedError,
  createRequestOwner,
  fetchNrfLogs,
  retireRequestOwner,
} from '../../api/deviceApi';
import type { DeviceStatus, NrfLogCounters, NrfLogNodeId, NrfLogNodeSummary, NrfLogRecord } from '../../domain/device';
import { useLocale, useT, type I18nKey } from '../../i18n';
import { formatNumber } from '../../i18n/format';
import { NRF_LOG_EVENTS } from './nrfLogEvents.gen';
import { formatEventArgs, formatEventArgsText, NRF_LOG_ARG_I18N_KEYS } from './nrfLogArgs';

const severityKeys = [
  'nrf_log.severity.debug',
  'nrf_log.severity.info',
  'nrf_log.severity.warning',
  'nrf_log.severity.error',
  'nrf_log.severity.fatal',
] as const satisfies readonly I18nKey[];

const subsystemKeys: Record<number, I18nKey> = {
  1: 'nrf_log.subsystem.boot',
  2: 'nrf_log.subsystem.log',
  3: 'nrf_log.subsystem.twis',
  4: 'nrf_log.subsystem.topology',
  5: 'nrf_log.subsystem.sensor',
  6: 'nrf_log.subsystem.calibration',
  7: 'nrf_log.subsystem.cs',
  8: 'nrf_log.subsystem.watchdog',
  9: 'nrf_log.subsystem.power',
};

export const NRF_LOG_PANEL_I18N_KEYS = [
  'nrf_log.panel.title', 'nrf_log.panel.node', 'nrf_log.panel.main', 'nrf_log.panel.no_nodes',
  'nrf_log.panel.no_records', 'nrf_log.panel.unsupported', 'nrf_log.panel.request_failed',
  'nrf_log.panel.minimum_severity', 'nrf_log.panel.severity', 'nrf_log.panel.subsystem',
  'nrf_log.panel.event', 'nrf_log.panel.esp_rx_uptime', 'nrf_log.panel.node_uptime',
  'nrf_log.panel.arguments', 'nrf_log.panel.arg0', 'nrf_log.panel.arg1', 'nrf_log.panel.export_json',
  'nrf_log.panel.export_csv', 'nrf_log.panel.counter_not_reported', 'nrf_log.panel.loading',
  'nrf_log.panel.node_select_aria', 'nrf_log.panel.severity_filter_aria', 'nrf_log.panel.table_aria',
  'nrf_log.panel.unknown_event', 'nrf_log.notices.reset', 'nrf_log.notices.node_rebooted',
  ...NRF_LOG_ARG_I18N_KEYS,
  ...severityKeys,
  ...Object.values(subsystemKeys),
  'nrf_log.counters.overwritten_on_node', 'nrf_log.counters.lost_inside_node_logger',
  'nrf_log.counters.wire_gaps', 'nrf_log.counters.dropped_on_esp',
] as const satisfies readonly I18nKey[];

function nodeChannel(node: NrfLogNodeId): number | null {
  if (node === 'main') return null;
  return Number(node.slice('remote'.length));
}

function nodeLabel(node: NrfLogNodeId, t: (key: I18nKey, params?: Readonly<Record<string, string | number>>) => string, locale: 'en' | 'uk'): string {
  const channel = nodeChannel(node);
  return channel === null
    ? t('nrf_log.panel.main')
    : t('sensors.pod', { number: formatNumber(locale, channel + 1) });
}

function eventText(record: NrfLogRecord, t: (key: I18nKey, params?: Readonly<Record<string, string | number>>) => string): string {
  const event = NRF_LOG_EVENTS[record.event_id as keyof typeof NRF_LOG_EVENTS];
  return event === undefined
    ? t('nrf_log.panel.unknown_event', { id: record.event_id })
    : t(`nrf_log.event.${event.name.toLowerCase()}` as I18nKey);
}

function subsystemText(subsystem: number, t: (key: I18nKey) => string): string {
  const key = subsystemKeys[subsystem];
  return key === undefined ? `#${subsystem}` : t(key);
}

export function nrfLogsJson(records: readonly NrfLogRecord[]): string {
  return JSON.stringify(records, null, 2);
}

export function nrfLogsCsv(records: readonly NrfLogRecord[], t: (key: I18nKey) => string): string {
  const fields = ['token', 'boot_id', 'seq', 'uptime_ms', 'esp_rx_uptime_ms', 'event_id', 'severity', 'subsystem', 'arg0', 'arg1'] as const;
  const quote = (value: string | number): string => `"${String(value).replaceAll('"', '""')}"`;
  return [
    [...fields, 'arguments_decoded'].map(quote).join(','),
    ...records.map((record) => [
      ...fields.map((field) => record[field]),
      formatEventArgsText(formatEventArgs(record.event_id, record.arg0, record.arg1), t),
    ].map(quote).join(',')),
  ].join('\r\n');
}

function download(text: string, mimeType: string, filename: string): void {
  const url = URL.createObjectURL(new Blob([text], { type: mimeType }));
  const anchor = document.createElement('a');
  anchor.href = url;
  anchor.download = filename;
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
  URL.revokeObjectURL(url);
}

function getNodeSummaries(status: DeviceStatus | null): NrfLogNodeSummary[] {
  return (status?.nrf_logs ?? [])
    .filter((summary) => summary.present || summary.stored > 0)
    .sort((left, right) => left.node === 'main' ? -1 : right.node === 'main' ? 1 : nodeChannel(left.node)! - nodeChannel(right.node)!);
}

export default function NrfLogPanel({ status }: { status: DeviceStatus | null }) {
  const t = useT();
  const { locale } = useLocale();
  const nodes = useMemo(() => getNodeSummaries(status), [status?.nrf_logs]);
  const nodeIds = nodes.map((summary) => summary.node);
  const nodeKey = nodeIds.join(',');
  const [selectedNode, setSelectedNode] = useState<NrfLogNodeId | null>(null);
  const activeNode = selectedNode !== null && nodeIds.includes(selectedNode) ? selectedNode : nodeIds[0] ?? null;
  const summary = nodes.find((item) => item.node === activeNode);
  const summaryUnsupported = summary?.logs_unsupported === true;
  const [visible, setVisible] = useState(() => typeof document === 'undefined' || document.visibilityState !== 'hidden');
  const [records, setRecords] = useState<NrfLogRecord[]>([]);
  const recordsRef = useRef<NrfLogRecord[]>([]);
  const afterTokenRef = useRef<string | undefined>(undefined);
  const bootIdRef = useRef<number | null | undefined>(undefined);
  const [rebootPositions, setRebootPositions] = useState<number[]>([]);
  const [counters, setCounters] = useState<NrfLogCounters | null>(null);
  const [pageUnsupported, setPageUnsupported] = useState(false);
  const [resetNotice, setResetNotice] = useState(false);
  const [requestFailed, setRequestFailed] = useState(false);
  const [loading, setLoading] = useState(false);
  const [minimumSeverity, setMinimumSeverity] = useState(0);

  useEffect(() => {
    if (selectedNode === null && nodeIds.length > 0) setSelectedNode(nodeIds[0]);
    else if (selectedNode !== null && !nodeIds.includes(selectedNode)) setSelectedNode(nodeIds[0] ?? null);
  }, [nodeKey, selectedNode]);

  useEffect(() => {
    recordsRef.current = [];
    afterTokenRef.current = undefined;
    bootIdRef.current = undefined;
    setRecords([]);
    setRebootPositions([]);
    setCounters(null);
    setPageUnsupported(false);
    setResetNotice(false);
    setRequestFailed(false);
    setLoading(activeNode !== null && !summaryUnsupported);
  }, [activeNode]);

  useEffect(() => {
    if (typeof document === 'undefined') return undefined;
    const onVisibilityChange = (): void => setVisible(document.visibilityState !== 'hidden');
    document.addEventListener('visibilitychange', onVisibilityChange);
    return () => document.removeEventListener('visibilitychange', onVisibilityChange);
  }, []);

  useEffect(() => {
    if (activeNode === null || !visible || summaryUnsupported || pageUnsupported) return undefined;
    let cancelled = false;
    let timer: number | null = null;
    const owner = createRequestOwner();

    const poll = async (): Promise<void> => {
      let result: Awaited<ReturnType<typeof fetchNrfLogs>>;
      try {
        result = await fetchNrfLogs(activeNode, afterTokenRef.current, 200, { owner });
      } catch (reason) {
        if (cancelled || reason instanceof ApiRequestOwnerRetiredError) return;
        setLoading(false);
        if (!(reason instanceof ApiRequestPreemptedError)) setRequestFailed(true);
        if (!cancelled) timer = window.setTimeout(() => void poll(), 2000);
        return;
      }
      if (cancelled) return;
      setLoading(false);
      if (!result.ok) {
        setRequestFailed(true);
      } else if (result.value.logs_unsupported) {
        setRequestFailed(false);
        setPageUnsupported(true);
      } else {
        const page = result.value;
        setRequestFailed(false);
        setCounters(page.counters);
        const bootChanged = bootIdRef.current !== undefined && page.boot_id !== null && bootIdRef.current !== page.boot_id;
        if (page.reset) {
          recordsRef.current = page.records;
          setRecords(page.records);
          setRebootPositions(bootChanged ? [0] : []);
          setResetNotice(true);
        } else {
          if (bootChanged) {
            const position = recordsRef.current.length;
            setRebootPositions((previous) => previous.includes(position) ? previous : [...previous, position]);
          }
          const nextRecords = [...recordsRef.current, ...page.records];
          recordsRef.current = nextRecords;
          setRecords(nextRecords);
        }
        if (page.boot_id !== null) bootIdRef.current = page.boot_id;
        afterTokenRef.current = page.next_token ?? page.records.at(-1)?.token ?? afterTokenRef.current;
      }
      if (!cancelled) timer = window.setTimeout(() => void poll(), 2000);
    };

    setLoading(recordsRef.current.length === 0 && afterTokenRef.current === undefined);
    void poll();
    return () => {
      cancelled = true;
      if (timer !== null) window.clearTimeout(timer);
      retireRequestOwner(owner);
    };
  }, [activeNode, visible, summaryUnsupported, pageUnsupported]);

  const unsupported = summaryUnsupported || pageUnsupported;
  const visibleRecords = records.filter((record) => record.severity >= minimumSeverity);
  const separators = new Set(rebootPositions);
  for (let index = 1; index < records.length; index += 1) {
    if (records[index - 1].boot_id !== records[index].boot_id) separators.add(index);
  }
  const summaryCounters = {
    overwritten_total: summary?.overwritten_total,
    lost_internal_total: undefined,
    wire_gap_total: summary?.wire_gap_total,
    local_drop_total: summary?.local_drop_total,
  };
  const shownCounters = counters ?? summaryCounters;
  const counterRows: Array<[I18nKey, number | undefined]> = [
    ['nrf_log.counters.overwritten_on_node', shownCounters.overwritten_total],
    ['nrf_log.counters.lost_inside_node_logger', shownCounters.lost_internal_total],
    ['nrf_log.counters.wire_gaps', shownCounters.wire_gap_total],
    ['nrf_log.counters.dropped_on_esp', shownCounters.local_drop_total],
  ];

  const exportRecords = (format: 'json' | 'csv'): void => {
    if (activeNode === null) return;
    const csv = format === 'csv';
    download(
      csv ? nrfLogsCsv(records, t) : nrfLogsJson(records),
      csv ? 'text/csv;charset=utf-8' : 'application/json',
      `nrf-logs-${activeNode}.${format}`,
    );
  };

  return (
    <article className="console-card wide-card nrf-log-card">
      <div className="card-heading">
        <h2>{t('nrf_log.panel.title')}</h2>
        <div className="nrf-log-actions">
          <button className="secondary" type="button" disabled={records.length === 0} onClick={() => exportRecords('json')}>{t('nrf_log.panel.export_json')}</button>
          <button className="secondary" type="button" disabled={records.length === 0} onClick={() => exportRecords('csv')}>{t('nrf_log.panel.export_csv')}</button>
        </div>
      </div>

      {nodes.length === 0 ? <p>{t('nrf_log.panel.no_nodes')}</p> : <>
        <div className="nrf-log-controls">
          <label>{t('nrf_log.panel.node')}
            <select aria-label={t('nrf_log.panel.node_select_aria')} value={activeNode ?? ''} onChange={(event) => setSelectedNode(event.target.value as NrfLogNodeId)}>
              {nodes.map((item) => <option key={item.node} value={item.node}>{nodeLabel(item.node, t, locale)}</option>)}
            </select>
          </label>
          <label>{t('nrf_log.panel.minimum_severity')}
            <select aria-label={t('nrf_log.panel.severity_filter_aria')} value={minimumSeverity} onChange={(event) => setMinimumSeverity(Number(event.target.value))}>
              {severityKeys.map((key, severity) => <option key={key} value={severity}>{t(key)}</option>)}
            </select>
          </label>
        </div>

        <dl className="nrf-log-counters">
          {counterRows.map(([key, value]) => <div key={key}><dt>{t(key)}</dt><dd>{value === undefined ? t('nrf_log.panel.counter_not_reported') : formatNumber(locale, value)}</dd></div>)}
        </dl>

        {resetNotice && <p className="console-alert warning" role="status">{t('nrf_log.notices.reset')}</p>}
        {unsupported && <p className="console-alert" role="status">{t('nrf_log.panel.unsupported')}</p>}
        {requestFailed && <p className="console-error" role="alert">{t('nrf_log.panel.request_failed')}</p>}
        {!unsupported && loading && records.length === 0 && <p>{t('nrf_log.panel.loading')}</p>}

        <div className="nrf-log-table-wrap">
          <table className="leveling-table nrf-log-table" aria-label={t('nrf_log.panel.table_aria')}>
            <thead><tr>
              <th>{t('nrf_log.panel.esp_rx_uptime')}</th>
              <th>{t('nrf_log.panel.node_uptime')}</th>
              <th>{t('nrf_log.panel.severity')}</th>
              <th>{t('nrf_log.panel.subsystem')}</th>
              <th>{t('nrf_log.panel.event')}</th>
              <th>{t('nrf_log.panel.arguments')}</th>
            </tr></thead>
            <tbody>
              {records.length === 0 && !loading && !unsupported && !requestFailed && <tr><td colSpan={6}>{t('nrf_log.panel.no_records')}</td></tr>}
              {Array.from({ length: records.length + 1 }, (_, index) => {
                const record = records[index];
                const rows = [];
                if (separators.has(index)) rows.push(<tr className="nrf-log-reboot" key={`reboot-${index}`}><td colSpan={6}>{t('nrf_log.notices.node_rebooted')}</td></tr>);
                if (record !== undefined && record.severity >= minimumSeverity) {
                  const args = formatEventArgs(record.event_id, record.arg0, record.arg1);
                  rows.push(<tr key={record.token}>
                    <td>{formatNumber(locale, record.esp_rx_uptime_ms)} ms</td>
                    <td>{formatNumber(locale, record.uptime_ms)} ms</td>
                    <td><span className={`nrf-log-severity severity-${record.severity}`}>{t(severityKeys[record.severity] ?? severityKeys[0])}</span></td>
                    <td>{subsystemText(record.subsystem, t)}</td>
                    <td>{eventText(record, t)}</td>
                    <td title={t('nrf_log.arg.raw_values', { arg0: args[0].raw, arg1: args[1].raw })}>{formatEventArgsText(args, t)}</td>
                  </tr>);
                }
                return rows;
              })}
              {records.length > 0 && visibleRecords.length === 0 && !unsupported && <tr><td colSpan={6}>{t('nrf_log.panel.no_records')}</td></tr>}
            </tbody>
          </table>
        </div>
      </>}
    </article>
  );
}
