import { lazy, Suspense, useEffect, useState } from 'react';
import type {
  DeviceStatus,
  ObjectStoreState,
  PhoneLocationInput,
  RecordingSessionCollection,
} from '../../domain/device';
import { formatCoordinate, normalizeCableLength } from '../../domain/device';
import {
  measurementControlLocked,
  measurementOwnsRecorder,
} from './measurementTransition';
import { recordingGateBlocksStart as isRecordingGateBlockingStart } from './recordingGate';
import '../seismic/SavedSeismicViewer.css';

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
  /* MAX-167: a 3D scan belongs to one recording/session, never the object
   * directly -- the caller must know which session it's attaching to. */
  onOpenScanner: (objectId: string, sessionId: string) => void;
}

function formatDuration(milliseconds: number | undefined): string {
  const seconds = Math.max(0, Math.floor((milliseconds ?? 0) / 1000));
  const minutes = Math.floor(seconds / 60);
  const remainder = seconds % 60;
  return `${minutes}:${remainder.toString().padStart(2, '0')}`;
}

export default function ObjectsFeature(props: Props) {
  const { status, store, sessions, selectedId, busy, error } = props;
  const selected = store?.objects.find((object) => object.id === selectedId)
    ?? store?.objects.find((object) => object.active)
    ?? null;
  const [newName, setNewName] = useState('');
  const [rename, setRename] = useState(selected?.name ?? '');
  const [cables, setCables] = useState<number[]>(selected?.cable_lengths_m ?? [5, 5, 5, 5]);
  const [defaults, setDefaults] = useState<number[]>(store?.default_cable_lengths_m ?? [5, 5, 5, 5]);
  const [showLive, setShowLive] = useState(false);
  const [openSessionId, setOpenSessionId] = useState<string | null>(null);
  const [showCalculate, setShowCalculate] = useState(false);
  const [gpsMessage, setGpsMessage] = useState<string | null>(null);

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
          <div><span className="eyebrow">DATABASE</span><h2>Objects</h2></div>
          <span>{store?.objects.length ?? 0}</span>
        </div>
        <form
          className="inline-form"
          onSubmit={(event) => {
            event.preventDefault();
            const name = newName.trim();
            void props.onCreate(name).then(() => setNewName(''));
          }}
        >
          <input value={newName} onChange={(event) => setNewName(event.target.value)} placeholder="New object name" maxLength={31} />
          <button disabled={busy}>Create</button>
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
              {object.active && <i>ACTIVE</i>}
            </button>
          ))}
          {store?.available === false && <p>SD/object database unavailable.</p>}
        </div>

        <details className="settings-details">
          <summary>Global cable defaults</summary>
          <p>New objects copy these values. Existing objects retain their object-specific cable lengths.</p>
          {defaults.map((length, index) => (
            <label key={index}>
              Pod {index + 1}
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
            <h2>Select an object</h2>
            <p>Create or select an SD object to bind GPS, configure sensor geometry and start measurements.</p>
          </article>
        ) : (
          <>
            <article className="console-card">
              <div className="card-heading">
                <div><span className="eyebrow">OBJECT {selected.id}</span><h1>{selected.name}</h1></div>
                <span className={`state-pill ${selected.active ? 'ready' : 'offline'}`}>{selected.active ? 'ACTIVE' : 'INACTIVE'}</span>
              </div>
              <div className="inline-form">
                <input value={rename} maxLength={31} onChange={(event) => setRename(event.target.value)} />
                <button disabled={busy || rename.trim().length === 0 || rename.trim() === selected.name} onClick={() => void props.onRename(selected.id, rename.trim())}>Rename</button>
              </div>
              <div className="console-actions">
                {!selected.active && <button disabled={busy} onClick={() => void props.onActivate(selected.id)}>Set active</button>}
                <button
                  disabled={busy || selectedRecording}
                  className="danger"
                  title={selectedRecording ? 'Stop the active recording before deleting this object.' : undefined}
                  onClick={() => { if (window.confirm(`Delete ${selected.name}?`)) void props.onDelete(selected.id); }}
                >Delete</button>
              </div>
            </article>

            <article className="console-card">
              <div className="card-heading">
                <div><span className="eyebrow">BASE STATION</span><h2>Phone GPS anchor</h2></div>
                <button
                  disabled={busy}
                  onClick={() => {
                    setGpsMessage('Requesting high-accuracy phone position…');
                    void props.onBindGps(selected.id)
                      .then((position) => setGpsMessage(`Saved ±${position.accuracy_m.toFixed(1)} m`))
                      .catch((reason) => setGpsMessage(reason instanceof Error ? reason.message : String(reason)));
                  }}
                >Bind current GPS</button>
              </div>
              {selected.location.valid ? (
                <dl className="console-dl">
                  <div><dt>Latitude</dt><dd>{formatCoordinate(selected.location.latitude)}</dd></div>
                  <div><dt>Longitude</dt><dd>{formatCoordinate(selected.location.longitude)}</dd></div>
                  <div><dt>Accuracy</dt><dd>±{selected.location.accuracy_m.toFixed(1)} m</dd></div>
                  <div><dt>Altitude</dt><dd>{selected.location.altitude_valid ? `${selected.location.altitude_m.toFixed(1)} m` : 'not reported by phone'}</dd></div>
                  <div><dt>Source</dt><dd>{selected.location.source}</dd></div>
                </dl>
              ) : <p>No global base coordinate is bound to this object.</p>}
              {gpsMessage !== null && <small>{gpsMessage}</small>}
            </article>

            <article className="console-card">
              <div className="card-heading">
                <div><span className="eyebrow">GEOMETRY</span><h2>Sensor cable lengths</h2></div>
                <small>object override</small>
              </div>
              <div className="cable-grid">
                {cables.map((length, index) => (
                  <label key={index}>
                    <span>Pod {index + 1}</span>
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
                    <button disabled={busy} onClick={() => void props.onCable(selected.id, index + 1, normalizeCableLength(cables[index]))}>Save</button>
                  </label>
                ))}
              </div>
            </article>

            <article className="console-card">
              <div className="card-heading">
                <div><span className="eyebrow">SEISMIC</span><h2>Acquisition & recording</h2></div>
                <span className={`state-pill ${selectedRecording ? 'ready' : status?.acquisition.running ? 'ready' : 'offline'}`}>
                  {selectedRecording ? 'RECORDING' : status?.acquisition.running ? 'LIVE' : 'STOPPED'}
                </span>
              </div>
              <p>Live acquisition stays independent from SD recording. Record drains the lock-free ring in bounded batches, so FAT/SD latency never becomes part of the acquisition producer hot path.</p>

              {selectedRecording && (
                <dl className="console-dl">
                  <div><dt>Session</dt><dd>{recording?.session_id ?? '—'}</dd></div>
                  <div><dt>Elapsed</dt><dd>{formatDuration(recording?.recording_ms)}</dd></div>
                  <div><dt>Written</dt><dd>{(recording?.samples_written ?? 0).toLocaleString()} frames</dd></div>
                  <div><dt>Ring drops</dt><dd>{(recording?.dropped_frames ?? 0).toLocaleString()}</dd></div>
                </dl>
              )}

              {anotherRecording && (
                <div className="console-alert">
                  Recording {recording?.session_id ?? ''} belongs to object {recording?.object_id}. Stop it before starting another object.
                </div>
              )}

              {recordingGateBlocksStart && (
                <div className="console-alert" role="alert">
                  Record disabled: {recordingGate?.reason}
                  {' '}({recordingGate?.assigned_count ?? 0} assigned, {recordingGate?.accepted_count ?? 0} level)
                </div>
              )}

              {recordingGate?.decision === 'ALLOWED_PLANAR_2D' && (
                <div className="console-alert warning" role="status">
                  {recordingGate.warning}
                </div>
              )}

              <div className="console-actions">
                <button disabled={busy || status?.acquisition.running === true} onClick={() => void props.onAcquisition(true)}>Start acquisition</button>
                <button
                  disabled={busy || status?.acquisition.running !== true || measurementLocked}
                  onClick={() => void props.onAcquisition(false)}
                >Stop acquisition</button>
                <button
                  className="secondary"
                  disabled={!liveAvailable}
                  title={liveAvailable ? undefined : 'Start acquisition before opening the live waveform.'}
                  onClick={() => setShowLive((value) => !value)}
                >{showLive ? 'Hide' : 'Show'} live signal</button>
                <button
                  disabled={recordingToggleDisabled}
                  className={selectedRecording ? 'danger' : undefined}
                  onClick={() => void props.onRecording(selected.id, !selectedRecording)}
                >
                  {selectedRecording ? 'Stop recording' : 'Record session'}
                </button>
              </div>
            </article>

            <article className="console-card">
              <div className="card-heading">
                <div><span className="eyebrow">SESSIONS</span><h2>Saved measurements</h2></div>
                <span>{selectedSessions.length}</span>
              </div>
              {sessions?.available === false && sessions.object_id === selected.id ? (
                <p>Session list unavailable: {sessions.error ?? 'storage error'}</p>
              ) : selectedSessions.length === 0 ? (
                <p>No saved geophone sessions for this object yet.</p>
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
                        title={readable ? 'Open saved waveform viewer' : 'Only terminal readable sessions can be viewed'}
                        onClick={() => setOpenSessionId(session.session_id)}
                      >
                        <span>
                          <strong>{session.session_id}</strong>
                          <small>{session.sample_rate_hz.toLocaleString()} Hz · {session.channel_count} ch · {session.recording_ms_available === false ? 'duration unknown' : formatDuration(session.recording_ms)}</small>
                        </span>
                        <span>
                          <strong>{session.samples_written.toLocaleString()}</strong>
                          <small>
                            {session.dropped_frames_available === false
                              ? `${statusLabel} · drops unknown`
                              : session.dropped_frames > 0
                                ? `${statusLabel} · ${session.dropped_frames.toLocaleString()} dropped`
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
                <article className="console-card">
                  <div className="card-heading">
                    <div><span className="eyebrow">SESSION {openSession.session_id}</span><h2>3D scan</h2></div>
                  </div>
                  <p>A 3D scan belongs to this recording, not the object -- open it here to add or review the scan captured for session {openSession.session_id}.</p>
                  <div className="console-actions">
                    <button
                      disabled={busy}
                      className="secondary"
                      onClick={() => props.onOpenScanner(selected.id, openSession.session_id)}
                    >Open 3D scan</button>
                    <button
                      disabled={busy}
                      className="secondary"
                      onClick={() => setShowCalculate((value) => !value)}
                    >{showCalculate ? 'Hide' : 'Calculate source'}</button>
                  </div>
                </article>
                {showCalculate && (
                  <Suspense fallback={<div className="console-card">Loading localization module…</div>}>
                    <LocalizationCalculatePanel
                      objectId={selected.id}
                      sessionId={openSession.session_id}
                      busy={busy}
                      onClose={() => setShowCalculate(false)}
                    />
                  </Suspense>
                )}
                <Suspense fallback={<div className="console-card">Loading saved waveform module…</div>}>
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
              <Suspense fallback={<div className="console-card">Loading waveform module…</div>}>
                <LiveSeismicPanel />
              </Suspense>
            )}
          </>
        )}
      </div>
    </section>
  );
}
