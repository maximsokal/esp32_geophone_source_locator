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
import { applyConfirmedObjectMutation } from './features/objects/objectMutation';
import {
  reconcileObjectSelection,
  type ObjectSelectionMode,
} from './features/objects/objectSelection';
import {
  measurementTransitionErrorMessage,
  measurementTransitionNeedsSessionRefresh,
  waitForStorageCacheIdle,
  waitForMeasurementTransition,
} from './features/objects/measurementTransition';
import SensorsFeature from './features/sensors/SensorsFeature';

const XrScannerFeature = lazy(() => import('./App'));

type Screen = 'dashboard' | 'objects' | 'sensors' | 'scanner';

const LOCAL_OBJECT_MUTATION_ACTIONS = new Set([
  'create_object',
  'activate_object',
  'delete_object',
  'rename_object',
  'set_object_cable_length',
  'set_default_cable_length',
  'set_object_location',
  'measurement_start',
]);

const MEASUREMENT_TRANSITION_ACTIONS = new Set(['measurement_start', 'measurement_stop']);

function errorMessage(reason: unknown): string {
  return reason instanceof Error ? reason.message : String(reason);
}

function transportFailure(message: string): boolean {
  return message.includes('network error') || message.includes('timeout');
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
  const sessionRefreshEpoch = useRef(0);
  const objectsRef = useRef<ObjectStoreState | null>(null);
  const reconcileTimer = useRef<number | null>(null);

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
    preferredSelectedId: string | null = null,
    selectionMode: ObjectSelectionMode = 'preserve',
  ): Promise<ObjectStoreState | null> => {
    const epoch = ++objectRefreshEpoch.current;
    try {
      const next = await fetchDeviceStatus(true);
      if (epoch !== objectRefreshEpoch.current) return null;

      const nextStore = next.object_store ?? null;
      setStatus(next);
      objectsRef.current = nextStore;
      setObjects(nextStore);
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
      setWorkspaceError(errorMessage(reason));
      return null;
    }
  }, []);

  const refreshSessions = useCallback(async (objectId: string | null): Promise<boolean> => {
    const epoch = ++sessionRefreshEpoch.current;
    if (objectId === null) {
      setSessions(null);
      return true;
    }

    try {
      /* Sessions are an SD model of one object; do not re-read every object profile. */
      const next = await fetchDeviceStatus(false, objectId);
      if (epoch !== sessionRefreshEpoch.current) return true;
      setStatus(next);
      setSessions(next.recording_sessions ?? null);
      setLinkError(null);
      return next.recording_sessions?.available !== false;
    }
    catch (reason) {
      if (epoch !== sessionRefreshEpoch.current) return false;
      setWorkspaceError(errorMessage(reason));
      return false;
    }
  }, []);

  const scheduleObjectReconcile = useCallback((selectionMode: ObjectSelectionMode): void => {
    if (reconcileTimer.current !== null) {
      window.clearTimeout(reconcileTimer.current);
    }
    reconcileTimer.current = window.setTimeout(() => {
      reconcileTimer.current = null;
      void refreshObjects(null, selectionMode);
    }, 1200);
  }, [refreshObjects]);

  /*
   * ESP HTTPS is intentionally low-concurrency. Poll slowly enough that status
   * remains useful without competing with SD-backed controls or session reads.
   * deviceApi serializes the actual fetches, so a poll waits behind a mutation
   * instead of opening another TLS request while the ESP is busy writing FATFS.
   */
  useEffect(() => {
    let cancelled = false;
    let timer: number | null = null;
    let failures = 0;

    const poll = async (): Promise<void> => {
      const ok = await refreshStatus();
      if (cancelled) return;
      failures = ok ? 0 : Math.min(failures + 1, 4);
      const delay = ok ? 5000 : Math.min(30000, 5000 * (2 ** failures));
      timer = window.setTimeout(() => void poll(), delay);
    };

    timer = window.setTimeout(() => void poll(), 5000);
    return () => {
      cancelled = true;
      if (timer !== null) window.clearTimeout(timer);
    };
  }, [refreshStatus]);

  /* Populate Home/Objects from one extended request. */
  useEffect(() => {
    void refreshObjects();
  }, [refreshObjects]);

  /*
   * Reconcile the complete object database infrequently. Confirmed web writes
   * are projected into React state immediately; this slow read is only for
   * physical-UI/other-client changes and recovery from an ambiguous transport
   * failure. Crucially, changing selectedId no longer triggers a full SD scan.
   */
  useEffect(() => {
    if (screen !== 'objects') return undefined;

    void refreshObjects();
    const slow = window.setInterval(() => void refreshObjects(), 60000);
    return () => window.clearInterval(slow);
  }, [refreshObjects, screen]);

  /* Selecting an object needs only its session list, not all object profiles. */
  useEffect(() => {
    if (screen !== 'objects') return undefined;
    void refreshSessions(selectedId);
    return undefined;
  }, [refreshSessions, screen, selectedId]);

  useEffect(() => () => {
    if (reconcileTimer.current !== null) window.clearTimeout(reconcileTimer.current);
  }, []);

  const runControl = useCallback(async (
    action: string,
    payload: Record<string, unknown> = {},
  ): Promise<boolean> => {
    const followsActive = action === 'create_object' || action === 'activate_object';
    const selectionMode: ObjectSelectionMode = followsActive ? 'active' : 'preserve';

    setBusy(true);
    try {
      const result = await sendControl(action, payload);
      const currentStore = objectsRef.current;
      const projectedStore = applyConfirmedObjectMutation(currentStore, action, payload, result);
      if (projectedStore !== currentStore) {
        objectsRef.current = projectedStore;
        setObjects(projectedStore);
      }

      const preferredSelectedId = action === 'create_object'
        ? result.object_id ?? null
        : (action === 'activate_object' || action === 'measurement_start')
          && typeof payload.object_id === 'string'
          ? payload.object_id
          : null;

      if (preferredSelectedId !== null) {
        setSelectedId(preferredSelectedId);
      }
      else if (action === 'delete_object' && projectedStore !== null) {
        setSelectedId((current) => reconcileObjectSelection(projectedStore, current));
      }

      setWorkspaceError(null);
      setLinkError(null);

      if (MEASUREMENT_TRANSITION_ACTIONS.has(action)) {
        /*
         * measurement_start/stop only enqueue work. Do not read the SD-backed
         * session model on the HTTP 200 boundary: STARTING/STOPPING may still
         * be draining, syncing, closing, and writing metadata. The status
         * endpoint exposes the measurement service's terminal transition.
         */
        const transitionStatus = await waitForMeasurementTransition(
          action as 'measurement_start' | 'measurement_stop',
          () => fetchDeviceStatus(false),
        );
        setStatus(transitionStatus);
        if (measurementTransitionNeedsSessionRefresh(
          action as 'measurement_start' | 'measurement_stop',
        )) {
          /* REVIEW means recorder finalization is complete, but the
           * asynchronous SD-backed status-cache refresh may still be between
           * invalidate and publish. Wait on fast RAM status before asking for
           * the cached session list, otherwise the expected 503 becomes a UI
           * error immediately after every STOP. */
          const cacheIdleStatus = await waitForStorageCacheIdle(
            () => fetchDeviceStatus(false),
          );
          setStatus(cacheIdleStatus);
          const confirmedObjectId = selectedId
            ?? transitionStatus.measurement?.object_id
            ?? transitionStatus.recording?.object_id
            ?? null;
          await refreshSessions(confirmedObjectId);
        }
        const transitionError = measurementTransitionErrorMessage(transitionStatus);
        if (transitionError !== null) {
          throw new Error(`ESP ${action} failed: ${transitionError}`);
        }
      }
      else if (!LOCAL_OBJECT_MUTATION_ACTIONS.has(action)) {
        await refreshStatus();
      }

      return true;
    }
    catch (reason) {
      const message = errorMessage(reason);

      /*
       * Never replay a write whose reply was lost: it might already be durable
       * on SD. A delayed authoritative GET is safe and, unlike the old code,
       * does not block the button handler or race the fast status poll.
       */
      if (transportFailure(message) && LOCAL_OBJECT_MUTATION_ACTIONS.has(action)) {
        scheduleObjectReconcile(selectionMode);
      }
      else if (action === 'measurement_start') {
        /* measurement_start activates before the worker can reject the request. */
        scheduleObjectReconcile('active');
      }

      setWorkspaceError(message);
      if (transportFailure(message)) setLinkError(message);
      return false;
    }
    finally {
      setBusy(false);
    }
  }, [refreshSessions, refreshStatus, scheduleObjectReconcile, selectedId]);

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
    const scannerObjectId = selectedId ?? objects?.active_id ?? null;
    return (
      <div className="scanner-feature-shell">
        <button className="console-floating-back" onClick={() => setScreen('objects')}>← Object</button>
        <Suspense fallback={<main className="console-loading">Loading WebXR scanner…</main>}>
          <XrScannerFeature objectId={scannerObjectId} />
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
          onRefresh={() => { void refreshObjects(); }}
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
            await runControl('delete_object', { object_id: id });
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
            await runControl(running ? 'measurement_start' : 'measurement_stop', running ? { object_id: id } : {});
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
