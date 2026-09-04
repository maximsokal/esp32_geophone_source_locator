import { lazy, Suspense, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import './console.css';
import { capturePhoneLocation, fetchDeviceStatus, sendControl } from './api/deviceApi';
import type {
  DeviceStatus,
  ObjectStoreState,
  PhoneLocationInput,
  RecordingSessionCollection,
} from './domain/device';
import DeviceDashboard from './features/dashboard/DeviceDashboard';
import ObjectsFeature from './features/objects/ObjectsFeature';
import {
  reconcileObjectSelection,
  type ObjectSelectionMode,
} from './features/objects/objectSelection';
import SensorsFeature from './features/sensors/SensorsFeature';

const XrScannerFeature = lazy(() => import('./App'));

type Screen = 'dashboard' | 'objects' | 'sensors' | 'scanner';

const OBJECT_MUTATION_ACTIONS = new Set([
  'create_object',
  'activate_object',
  'delete_object',
  'rename_object',
  'set_object_cable_length',
  'set_default_cable_length',
  'set_object_location',
  'recording_start',
]);

const SESSION_REFRESH_ACTIONS = new Set(['recording_start', 'recording_stop']);

function errorMessage(reason: unknown): string {
  return reason instanceof Error ? reason.message : String(reason);
}

export default function DeviceConsoleApp() {
  const [screen, setScreen] = useState<Screen>('dashboard');
  const [status, setStatus] = useState<DeviceStatus | null>(null);
  const [objects, setObjects] = useState<ObjectStoreState | null>(null);
  const [sessions, setSessions] = useState<RecordingSessionCollection | null>(null);
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const [linkError, setLinkError] = useState<string | null>(null);
  const [workspaceError, setWorkspaceError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const objectRefreshEpoch = useRef(0);

  const refreshStatus = useCallback(async (): Promise<boolean> => {
    try {
      const next = await fetchDeviceStatus(false);
      setStatus(next);
      setLinkError(null);
      return true;
    }
    catch (reason) {
      setLinkError(errorMessage(reason));
      return false;
    }
  }, []);

  const refreshObjects = useCallback(async (
    sessionsObjectId: string | null = null,
    preferredSelectedId: string | null = null,
    selectionMode: ObjectSelectionMode = 'preserve',
  ): Promise<ObjectStoreState | null> => {
    const epoch = ++objectRefreshEpoch.current;
    try {
      const next = await fetchDeviceStatus(true, sessionsObjectId);
      if (epoch !== objectRefreshEpoch.current) return null;

      const nextStore = next.object_store ?? null;
      setStatus(next);
      setObjects(nextStore);
      setSessions(next.recording_sessions ?? null);
      setSelectedId((current) => reconcileObjectSelection(
        nextStore,
        current,
        preferredSelectedId,
        selectionMode,
      ));
      setLinkError(null);
      setWorkspaceError(nextStore?.available === false
        ? `Object database unavailable: ${nextStore.error ?? 'storage error'}`
        : null);
      return nextStore?.available === false ? null : nextStore;
    }
    catch (reason) {
      if (epoch !== objectRefreshEpoch.current) return null;
      const message = errorMessage(reason);
      setLinkError(message);
      setWorkspaceError(message);
      return null;
    }
  }, []);

  /*
   * The extended boot request already contains the RAM-backed status snapshot.
   * Start the fast poll only after the page has had time to load instead of
   * racing a second TLS request against the initial object/database request.
   * Back off on failures so an unstable Wi-Fi/TLS link cannot become a request
   * storm that prevents the next useful request from succeeding.
   */
  useEffect(() => {
    let cancelled = false;
    let timer: number | null = null;
    let failures = 0;

    const poll = async (): Promise<void> => {
      const ok = await refreshStatus();
      if (cancelled) return;
      failures = ok ? 0 : Math.min(failures + 1, 4);
      const delay = ok ? 2000 : Math.min(15000, 2000 * (2 ** failures));
      timer = window.setTimeout(() => void poll(), delay);
    };

    timer = window.setTimeout(() => void poll(), 2000);
    return () => {
      cancelled = true;
      if (timer !== null) window.clearTimeout(timer);
    };
  }, [refreshStatus]);

  /* Populate Home/Objects from one extended request. */
  useEffect(() => {
    void refreshObjects(null);
  }, [refreshObjects]);

  /*
   * Object metadata is cached by firmware. While this page is visible, a slow
   * 30 s reconciliation keeps physical-UI/other-client changes eventually
   * consistent without hammering FATFS on every status poll.
   */
  useEffect(() => {
    if (screen !== 'objects') return undefined;

    void refreshObjects(selectedId);
    const slow = window.setInterval(() => void refreshObjects(selectedId), 30000);
    return () => window.clearInterval(slow);
  }, [refreshObjects, screen, selectedId]);

  const runControl = useCallback(async (
    action: string,
    payload: Record<string, unknown> = {},
  ): Promise<boolean> => {
    const activeBefore = objects?.active_id ?? null;
    const followsActive = action === 'create_object' || action === 'activate_object';
    const selectionMode: ObjectSelectionMode = followsActive ? 'active' : 'preserve';

    setBusy(true);
    try {
      const result = await sendControl(action, payload);
      const preferredSelectedId = action === 'create_object'
        ? result.object_id ?? null
        : null;

      if (preferredSelectedId !== null) {
        setSelectedId(preferredSelectedId);
      }
      setWorkspaceError(null);

      if (OBJECT_MUTATION_ACTIONS.has(action) || SESSION_REFRESH_ACTIONS.has(action)) {
        const withSessions = screen === 'objects' && SESSION_REFRESH_ACTIONS.has(action)
          ? selectedId
          : null;
        await refreshObjects(withSessions, preferredSelectedId, selectionMode);
      }
      else {
        await refreshStatus();
      }
      return true;
    }
    catch (reason) {
      /*
       * Control writes are intentionally never retried: a TLS reset can happen
       * after the ESP has already committed the mutation. Re-read authoritative
       * state instead. For create/activate, following active is deliberate; an
       * ordinary periodic refresh still preserves an operator-selected inactive
       * object.
       */
      let reconciledStore: ObjectStoreState | null = null;
      if (OBJECT_MUTATION_ACTIONS.has(action) || SESSION_REFRESH_ACTIONS.has(action)) {
        const withSessions = screen === 'objects' && SESSION_REFRESH_ACTIONS.has(action)
          ? selectedId
          : null;
        reconciledStore = await refreshObjects(withSessions, null, selectionMode);
      }

      if (
        action === 'create_object'
        && reconciledStore !== null
        && reconciledStore.active_id !== null
        && reconciledStore.active_id !== activeBefore
      ) {
        /* The POST response was lost, but the subsequent GET proves create committed. */
        setWorkspaceError(null);
        setLinkError(null);
        return true;
      }

      if (
        action === 'activate_object'
        && typeof payload.object_id === 'string'
        && reconciledStore?.active_id === payload.object_id
      ) {
        setWorkspaceError(null);
        setLinkError(null);
        return true;
      }

      const message = errorMessage(reason);
      setWorkspaceError(message);
      if (message.includes('network error') || message.includes('timeout')) setLinkError(message);
      return false;
    }
    finally {
      setBusy(false);
    }
  }, [objects, refreshObjects, refreshStatus, screen, selectedId]);

  const selectedObject = useMemo(
    () => objects?.objects.find((object) => object.id === selectedId)
      ?? objects?.objects.find((object) => object.active)
      ?? null,
    [objects, selectedId],
  );

  const openActive = () => {
    const id = objects?.active_id ?? objects?.objects.find((object) => object.active)?.id;
    if (id !== undefined) setSelectedId(id);
    setScreen('objects');
  };

  if (screen === 'scanner') {
    return (
      <div className="scanner-feature-shell">
        <button className="console-floating-back" onClick={() => setScreen('objects')}>← Object</button>
        <Suspense fallback={<main className="console-loading">Loading WebXR scanner…</main>}>
          <XrScannerFeature />
        </Suspense>
      </div>
    );
  }

  const apiOnline = status !== null && linkError === null;
  const apiLabel = status === null && linkError === null ? 'CONNECTING' : apiOnline ? 'ESP LINK' : 'API ERROR';
  const pageError = workspaceError ?? linkError;

  return (
    <main className="device-console-shell">
      <header className="console-topbar">
        <button className="brand-button" onClick={() => setScreen('dashboard')}>
          <span>GEOLOCATOR</span>
          <small>{selectedObject?.name ?? 'ESP32-S3'}</small>
        </button>
        <nav>
          <button aria-pressed={screen === 'dashboard'} onClick={() => setScreen('dashboard')}>Home</button>
          <button aria-pressed={screen === 'objects'} onClick={() => setScreen('objects')}>Objects</button>
          <button aria-pressed={screen === 'sensors'} onClick={() => setScreen('sensors')}>Sensors</button>
        </nav>
        <div className={`device-dot ${apiOnline ? 'online' : 'offline'}`}>{apiLabel}</div>
      </header>

      {screen === 'dashboard' && (
        <DeviceDashboard
          status={status}
          objects={objects}
          error={pageError}
          onOpenObjects={() => setScreen('objects')}
          onOpenSensors={() => setScreen('sensors')}
          onOpenActiveObject={openActive}
          onRefresh={() => { void refreshObjects(null); }}
        />
      )}

      {screen === 'objects' && (
        <ObjectsFeature
          status={status}
          store={objects}
          sessions={sessions}
          selectedId={selectedId}
          busy={busy}
          error={pageError}
          onSelect={setSelectedId}
          onCreate={async (name) => { await runControl('create_object', name ? { name } : {}); }}
          onActivate={async (id) => { await runControl('activate_object', { object_id: id }); }}
          onRename={async (id, name) => { await runControl('rename_object', { object_id: id, name }); }}
          onDelete={async (id) => {
            if (await runControl('delete_object', { object_id: id })) setSelectedId(null);
          }}
          onBindGps={async (id): Promise<PhoneLocationInput> => {
            const position = await capturePhoneLocation();
            const saved = await runControl('set_object_location', { object_id: id, ...position });
            if (!saved) throw new Error('ESP did not save the phone GPS position. See the API error above.');
            return position;
          }}
          onCable={async (id, podId, length) => {
            await runControl('set_object_cable_length', { object_id: id, pod_id: podId, length_m: length });
          }}
          onDefaultCable={async (podId, length) => {
            await runControl('set_default_cable_length', { pod_id: podId, length_m: length });
          }}
          onAcquisition={async (running) => {
            await runControl(running ? 'acquisition_start' : 'acquisition_stop');
          }}
          onRecording={async (id, running) => {
            await runControl(running ? 'recording_start' : 'recording_stop', running ? { object_id: id } : {});
          }}
          onOpenScanner={(id) => { setSelectedId(id); setScreen('scanner'); }}
        />
      )}

      {screen === 'sensors' && (
        <SensorsFeature status={status} object={selectedObject} error={linkError} />
      )}
    </main>
  );
}
