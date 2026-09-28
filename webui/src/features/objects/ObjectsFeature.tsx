import RecordingEvidenceCard from './RecordingEvidenceCard';
import { lazy, Suspense, useEffect, useState } from 'react';
import type {
  DeviceStatus,
  ObjectStoreState,
  PhoneLocationInput,
  RecordingSessionCollection,
} from '../../domain/device';
import { activeObject, normalizeCableLength } from '../../domain/device';
import { useLocale, useT } from '../../i18n';
import { formatNumber } from '../../i18n/format';
import {
  measurementControlLocked,
  measurementOwnsRecorder,
} from './measurementTransition';
import { recordingGateBlocksStart as isRecordingGateBlockingStart } from './recordingGate';
import { recordingGateReason, recordingGateWarning } from './recordingGateUi';
import '../seismic/SavedSeismicViewer.css';
import '../workflow/workflow.css';

const LiveSeismicPanel = lazy(() => import('../seismic/LiveSeismicPanel'));
const SavedSeismicViewer = lazy(() => import('../seismic/SavedSeismicViewer'));
const LocalizationCalculatePanel = lazy(() => import('./LocalizationCalculatePanel'));

interface Props {
  status: DeviceStatus | null;
  store: ObjectStoreState | null;
  sessions: RecordingSessionCollection | null;
  selectedId: string | null;
  busy: boolean;
  error: string | null;
  onSelect: (id: string) => void;
  onCreate: (name: string) => Promise<void>;
  onActivate: (id: string) => Promise<void>;
  onRename: (id: string, name: string) => Promise<void>;
  onDelete: (id: string) => Promise<void>;
  onBindGps: (id: string) => Promise<PhoneLocationInput>;
  onCable: (id: string, podId: number, length: number) => Promise<void>;
  onDefaultCable: (podId: number, length: number) => Promise<void>;
  onAcquisition: (running: boolean) => Promise<void>;
  onRecording: (id: string, running: boolean) => Promise<void>;
  onStartWorkflow: (id: string) => void;
  /* MAX-167: a 3D scan belongs to one recording/session, never the object
   * directly -- the caller must know which session it's attaching to. */
  onOpenScanner: (objectId: string, sessionId: string) => void;
}

type GpsMessage = { kind: 'requesting' } | { kind: 'saved'; accuracy: number } | { kind: 'error'; message: string };

function formatDuration(milliseconds: number | undefined): string {
  const seconds = Math.max(0, Math.floor((milliseconds ?? 0) / 1000));
  const minutes = Math.floor(seconds / 60);
  const remainder = seconds % 60;
  return `${minutes}:${remainder.toString().padStart(2, '0')}`;
}

export default function ObjectsFeature(props: Props) {
  const { locale, selectedLocale } = useLocale();
  const t = useT();
  const { status, store, sessions, selectedId, busy, error } = props;
  const selected = store?.objects.find((object) => object.id === selectedId)
    ?? store?.objects.find((object) => object.active)
    ?? null;
  const active = activeObject(store);
  const [newName, setNewName] = useState('');
  const [rename, setRename] = useState(selected?.name ?? '');
  const [cables, setCables] = useState<number[]>(selected?.cable_lengths_m ?? [5, 5, 5, 5]);
  const [defaults, setDefaults] = useState<number[]>(store?.default_cable_lengths_m ?? [5, 5, 5, 5]);
  const [showLive, setShowLive] = useState(false);
  const [openSessionId, setOpenSessionId] = useState<string | null>(null);
  const [showCalculate, setShowCalculate] = useState(false);
  const [reportError, setReportError] = useState<string | null>(null);
  const [gpsMessage, setGpsMessage] = useState<GpsMessage | null>(null);

  useEffect(() => {
    setRename(selected?.name ?? '');
    setCables(selected?.cable_lengths_m ?? [5, 5, 5, 5]);
  }, [selected?.id, selected?.name, selected?.cable_lengths_m]);

  useEffect(() => {
    setOpenSessionId(null);
    setShowCalculate(false);
  }, [selected?.id]);

  useEffect(() => {
    setShowCalculate(false);
  }, [openSessionId]);

  useEffect(() => {
    setDefaults(store?.default_cable_lengths_m ?? [5, 5, 5, 5]);
  }, [store?.default_cable_lengths_m]);

  useEffect(() => {
    if (status?.acquisition.running !== true) setShowLive(false);
  }, [status?.acquisition.running]);

  const recording = status?.recording;
  const measurement = status?.measurement;
  const measurementOwnsTheRecorder = status !== null && measurementOwnsRecorder(status);
  const measurementLocked = status !== null && measurementControlLocked(status);
  const measurementObjectId = measurement?.object_id || recording?.object_id;
  const selectedRecording = selected !== null
    && measurementOwnsTheRecorder
    && measurementObjectId === selected.id;
  const anotherRecording = measurementOwnsTheRecorder && !selectedRecording;
  const recordingGate = status?.recording_gate;
  const gateReason = recordingGate === undefined ? null : recordingGateReason(recordingGate, t);
  const gateWarning = recordingGate === undefined ? null : recordingGateWarning(recordingGate, t);
  /* MAX-165: block starting a *new* recording when the gate says so (fewer
   * than 3 assigned REMOTEs, or an assigned REMOTE that is not yet level).
   * Never blocks stopping an already-running recording -- the gate is about
   * admission, not about an in-progress session. */
  const recordingGateBlocksStart = isRecordingGateBlockingStart(
    recordingGate,
    selectedRecording,
  );
  const recordingToggleDisabled =
    busy ||
    anotherRecording ||
    recordingGateBlocksStart ||
    (measurementLocked && measurement?.state !== 'RECORDING');
  const selectedSessions = selected !== null && sessions?.object_id === selected.id
    ? sessions.sessions
    : [];
  const openSession = selectedSessions.find((session) =>
    session.session_id === openSessionId && (session.readable ?? session.complete)) ?? null;
  const liveAvailable = status?.acquisition.running === true;

  return (
    <section className="console-page object-layout">
      {error !== null && <div className="console-alert" role="alert">{error}</div>}

      <aside className="console-card object-list-card">
        <div className="card-heading">
          <div><span className="eyebrow">{t('objects.database')}</span><h2>{t('nav.objects')}</h2></div>
          <span>{store?.objects.length ?? 0}</span>
        </div>
        {active !== null && (
          <div className="workflow-entry">
            <small>{active.name}</small>
            <button type="button" className="secondary" disabled={busy} onClick={() => props.onStartWorkflow(active.id)}>
              {t('workflow.entry_button')}
            </button>
          </div>
        )}
        <form
          className="inline-form"
          onSubmit={(event) => {
            event.preventDefault();
            const name = newName.trim();
            void props.onCreate(name).then(() => setNewName(''));
          }}
        >
          <input value={newName} onChange={(event) => setNewName(event.target.value)} placeholder={t('objects.new_name')} maxLength={31} />
          <button disabled={busy}>{t('objects.create')}</button>
        </form>

        <div className="object-list">
          {(store?.objects ?? []).map((object) => (
            <button
              type="button"
              key={object.id}
              className={`object-row ${selected?.id === object.id ? 'selected' : ''}`}
              onClick={() => props.onSelect(object.id)}
            >
              <span><strong>{object.name}</strong><small>{object.id}</small></span>
              {object.active && <i>{t('web.objects.active')}</i>}
            </button>
          ))}
          {store?.available === false && <p>{t('objects.store_unavailable')}</p>}
        </div>

        <details className="settings-details">
          <summary>{t('objects.defaults')}</summary>
          <p>{t('objects.defaults_note')}</p>
          {defaults.map((length, index) => (
            <label key={index}>
              {t('objects.pod', { number: index + 1 })}
              <input
                type="number"
                min="0.1"
                max="100"
                step="0.1"
                value={length}
                onChange={(event) => {
                  const next = [...defaults];
                  next[index] = Number(event.target.value);
                  setDefaults(next);
                }}
                onBlur={() => void props.onDefaultCable(index + 1, normalizeCableLength(defaults[index]))}
              /> m
            </label>
          ))}
        </details>
      </aside>

      <div className="object-workspace">
        {selected === null ? (
          <article className="console-card empty-card">
            <h2>{t('objects.select_title')}</h2>
            <p>{t('objects.select_note')}</p>
          </article>
        ) : (
          <>
            <article className="console-card">
              <div className="card-heading">
                <div><span className="eyebrow">{t('objects.object_label', { id: selected.id })}</span><h1>{selected.name}</h1></div>
                <span className={`state-pill ${selected.active ? 'ready' : 'offline'}`}>{selected.active ? t('web.objects.active') : t('objects.inactive')}</span>
              </div>
              <div className="inline-form">
                <input value={rename} maxLength={31} onChange={(event) => setRename(event.target.value)} />
                <button disabled={busy || rename.trim().length === 0 || rename.trim() === selected.name} onClick={() => void props.onRename(selected.id, rename.trim())}>{t('objects.rename')}</button>
              </div>
              <div className="console-actions">
                {!selected.active && <button disabled={busy} onClick={() => void props.onActivate(selected.id)}>{t('web.objects.set_active')}</button>}
                <button
                  disabled={busy || selectedRecording}
                  className="danger"
                  title={selectedRecording ? t('objects.delete_stop_first') : undefined}
                  onClick={() => { if (window.confirm(t('objects.delete_confirm', { name: selected.name }))) void props.onDelete(selected.id); }}
                >{t('web.objects.delete')}</button>
              </div>
            </article>

            <article className="console-card">
              <div className="card-heading">
                <div><span className="eyebrow">{t('objects.base_station')}</span><h2>{t('objects.phone_gps')}</h2></div>
                <button
                  disabled={busy}
                  onClick={() => {
                    setGpsMessage({ kind: 'requesting' });
                    void props.onBindGps(selected.id)
                      .then((position) => setGpsMessage({ kind: 'saved', accuracy: position.accuracy_m }))
                      .catch((reason) => setGpsMessage({ kind: 'error', message: reason instanceof Error ? reason.message : String(reason) }));
                  }}
                >{t('objects.bind_gps')}</button>
              </div>
              {selected.location.valid ? (
                <dl className="console-dl">
                  <div><dt>{t('objects.latitude')}</dt><dd>{formatNumber(locale, selected.location.latitude, { useGrouping: false, minimumFractionDigits: 6, maximumFractionDigits: 6 })}</dd></div>
                  <div><dt>{t('objects.longitude')}</dt><dd>{formatNumber(locale, selected.location.longitude, { useGrouping: false, minimumFractionDigits: 6, maximumFractionDigits: 6 })}</dd></div>
                  <div><dt>{t('objects.accuracy')}</dt><dd>±{formatNumber(locale, selected.location.accuracy_m, { minimumFractionDigits: 1, maximumFractionDigits: 1 })} m</dd></div>
                  <div><dt>{t('objects.altitude')}</dt><dd>{selected.location.altitude_valid ? `${formatNumber(locale, selected.location.altitude_m, { minimumFractionDigits: 1, maximumFractionDigits: 1 })} m` : t('objects.not_reported_phone')}</dd></div>
                  <div><dt>{t('objects.source')}</dt><dd>{selected.location.source}</dd></div>
                </dl>
              ) : <p>{t('objects.no_base_coordinate')}</p>}
              {gpsMessage !== null && <small>{gpsMessage.kind === 'requesting' ? t('objects.gps_requesting') : gpsMessage.kind === 'saved' ? t('objects.gps_saved', { accuracy: formatNumber(locale, gpsMessage.accuracy, { minimumFractionDigits: 1, maximumFractionDigits: 1 }) }) : gpsMessage.message}</small>}
            </article>

            <article className="console-card">
              <div className="card-heading">
                <div><span className="eyebrow">{t('objects.geometry')}</span><h2>{t('objects.cable_lengths')}</h2></div>
                <small>{t('objects.object_override')}</small>
              </div>
              <div className="cable-grid">
                {cables.map((length, index) => (
                  <label key={index}>
                    <span>{t('objects.pod', { number: index + 1 })}</span>
                    <input
                      type="number"
                      min="0.1"
                      max="100"
                      step="0.1"
                      value={length}
                      onChange={(event) => {
                        const next = [...cables];
                        next[index] = Number(event.target.value);
                        setCables(next);
                      }}
                    />
                    <button disabled={busy} onClick={() => void props.onCable(selected.id, index + 1, normalizeCableLength(cables[index]))}>{t('objects.save')}</button>
                  </label>
                ))}
              </div>
            </article>

            <article className="console-card">
              <div className="card-heading">
                <div><span className="eyebrow">{t('objects.seismic')}</span><h2>{t('objects.acquisition_recording')}</h2></div>
                <span className={`state-pill ${selectedRecording ? 'ready' : status?.acquisition.running ? 'ready' : 'offline'}`}>
                  {selectedRecording ? t('objects.recording') : status?.acquisition.running ? t('objects.live') : t('objects.stopped')}
                </span>
              </div>
              <p>{t('objects.recording_description')}</p>

              {selectedRecording && (
                <dl className="console-dl">
                  <div><dt>{t('objects.session')}</dt><dd>{recording?.session_id ?? '—'}</dd></div>
                  <div><dt>{t('objects.elapsed')}</dt><dd>{formatDuration(recording?.recording_ms)}</dd></div>
                  <div><dt>{t('objects.written')}</dt><dd>{formatNumber(locale, recording?.samples_written ?? 0)} {t('objects.frames')}</dd></div>
                  <div><dt>{t('objects.ring_drops')}</dt><dd>{formatNumber(locale, recording?.dropped_frames ?? 0)}</dd></div>
                </dl>
              )}

              {anotherRecording && (
                <div className="console-alert">
                  {t('objects.another_recording', { session: recording?.session_id ?? '', object: recording?.object_id ?? '' })}
                </div>
              )}

              {recordingGateBlocksStart && (
                <div className="console-alert" role="alert">
                  {t('objects.recording_disabled', { reason: gateReason ?? recordingGate?.decision ?? '', assigned: formatNumber(locale, recordingGate?.assigned_count ?? 0), accepted: formatNumber(locale, recordingGate?.accepted_count ?? 0) })}
                </div>
              )}

              {recordingGate?.decision === 'ALLOWED_PLANAR_2D' && (
                <div className="console-alert warning" role="status">
                  {gateWarning}
                </div>
              )}

              <div className="console-actions">
                <button disabled={busy || status?.acquisition.running === true} onClick={() => void props.onAcquisition(true)}>{t('objects.start_acquisition')}</button>
                <button
                  disabled={busy || status?.acquisition.running !== true || measurementLocked}
                  onClick={() => void props.onAcquisition(false)}
                >{t('objects.stop_acquisition')}</button>
                <button
                  className="secondary"
                  disabled={!liveAvailable}
                  title={liveAvailable ? undefined : t('objects.live_signal_start_first')}
                  onClick={() => setShowLive((value) => !value)}
                >{showLive ? t('objects.hide') : t('objects.show')} {t('objects.live_signal')}</button>
                <button
                  disabled={recordingToggleDisabled}
                  className={selectedRecording ? 'danger' : undefined}
                  onClick={() => void props.onRecording(selected.id, !selectedRecording)}
                >
                  {selectedRecording ? t('objects.stop_recording') : t('objects.record_session')}
                </button>
              </div>
            </article>

            <article className="console-card">
              <div className="card-heading">
                <div><span className="eyebrow">{t('objects.sessions')}</span><h2>{t('objects.saved_measurements')}</h2></div>
                <span>{selectedSessions.length}</span>
              </div>
              {sessions?.available === false && sessions.object_id === selected.id ? (
                <p>{t('objects.sessions_unavailable', { error: sessions.error ?? t('objects.storage_error') })}</p>
              ) : selectedSessions.length === 0 ? (
                <p>{t('objects.no_sessions')}</p>
              ) : (
                <div className="session-list">
                  {selectedSessions.map((session) => {
                    const readable = session.readable ?? session.complete;
                    const statusLabel = session.status ?? (session.complete ? 'complete' : 'incomplete');
                    return (
                      <button
                        type="button"
                        className="session-row"
                        key={session.session_id}
                        disabled={!readable || busy}
                          title={readable ? t('objects.open_waveform') : t('objects.unreadable_session')}
                        onClick={() => setOpenSessionId(session.session_id)}
                      >
                        <span>
                          <strong>{session.session_id}</strong>
                          <small>{formatNumber(locale, session.sample_rate_hz)} Hz · {formatNumber(locale, session.channel_count)} {t('objects.channels')} · {session.recording_ms_available === false ? t('objects.duration_unknown') : formatDuration(session.recording_ms)}</small>
                        </span>
                        <span>
                          <strong>{formatNumber(locale, session.samples_written)}</strong>
                          <small>
                            {session.dropped_frames_available === false
                              ? t('objects.drops_unknown', { status: statusLabel })
                              : session.dropped_frames > 0
                                ? t('objects.dropped', { status: statusLabel, count: formatNumber(locale, session.dropped_frames) })
                                : statusLabel}
                          </small>
                        </span>
                      </button>
                    );
                  })}
                </div>
              )}
            </article>

            {openSession !== null && (
              <>
                <RecordingEvidenceCard
                  objectId={selected.id}
                  sessionId={openSession.session_id}
                  refreshKey={showCalculate ? 1 : 0}
                />
                <article className="console-card">
                  <div className="card-heading">
                    <div><span className="eyebrow">{t('objects.session')} {openSession.session_id}</span><h2>{t('objects.scan')}</h2></div>
                  </div>
                  <p>{t('objects.scan_description', { session: openSession.session_id })}</p>
                  <div className="console-actions">
                    <button
                      disabled={busy}
                      className="secondary"
                      onClick={() => props.onOpenScanner(selected.id, openSession.session_id)}
                    >{t('objects.scan_add')}</button>
                    <button
                      disabled={busy}
                      className="secondary"
                      onClick={() => setShowCalculate((value) => !value)}
                    >{showCalculate ? t('objects.localization_hide') : t('objects.localization_open')}</button>
                    <button
                      disabled={busy}
                      className="secondary"
                      onClick={() => {
                        setReportError(null);
                        const localeAtAdmission = selectedLocale;
                        void import('../report/reportExport')
                          .then((module) => module.openRecordingReport(selected, openSession, null, localeAtAdmission))
                          .catch((reason: unknown) => setReportError(reason instanceof Error ? reason.message : String(reason)));
                      }}
                    >{t('objects.export_pdf')}</button>
                  </div>
                  {reportError !== null && <p className="overlay-note">{t('objects.report_error', { error: reportError })}</p>}
                </article>
                {showCalculate && (
                  <Suspense fallback={<div className="console-card">{t('objects.loading_localization')}</div>}>
                    <LocalizationCalculatePanel
                      objectId={selected.id}
                      sessionId={openSession.session_id}
                      busy={busy}
                      onClose={() => setShowCalculate(false)}
                    />
                  </Suspense>
                )}
                <Suspense fallback={<div className="console-card">{t('objects.loading_saved_waveform')}</div>}>
                  <SavedSeismicViewer
                    objectId={selected.id}
                    sessionId={openSession.session_id}
                    summary={openSession}
                    onClose={() => setOpenSessionId(null)}
                  />
                </Suspense>
              </>
            )}

            {showLive && liveAvailable && (
              <Suspense fallback={<div className="console-card">{t('objects.loading_waveform')}</div>}>
                <LiveSeismicPanel />
              </Suspense>
            )}
          </>
        )}
      </div>
    </section>
  );
}
