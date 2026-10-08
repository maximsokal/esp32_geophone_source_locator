import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import './console.css';
import {
  capturePhoneLocation,
  fetchDeviceStatus,
  isStorageAvailabilityError,
  sendControl,
  sendWorkflowCommand,
} from './api/deviceApi';
import { DeviceStatusValidationError } from './api/deviceStatusSchema';
import type {
  DeviceStatus,
  ObjectStoreState,
  PhoneLocationInput,
  RecordingSessionCollection,
} from './domain/device';
import DeviceDashboard from './features/dashboard/DeviceDashboard';
import ObjectsFeature from './features/objects/ObjectsFeature';
import { applyConfirmedObjectMutation } from './features/objects/objectMutation';
import { runAfterControlConfirmation } from './features/objects/controlConfirmation';
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
import PowerBanner from './components/PowerBanner';
import StorageUnavailableBanner from './components/StorageUnavailableBanner';
import { formatErrorText, TranslatedError, useLocale, useT } from './i18n';
import ScannerFeatureHost from './features/scanner/ScannerFeatureHost';
import WorkflowPanel from './features/workflow/WorkflowPanel';
import { workflowStatusPollDelay } from './features/workflow/workflowPolling';

type Screen = 'dashboard' | 'objects' | 'sensors' | 'workflow' | 'scanner';
const MAX_CONSOLE_ERROR_LENGTH = 200;

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

function transportFailure(message: string): boolean {
  return message.includes('network error') || message.includes('timeout');
}

function boundErrorText(message: string): string {
  return message.length <= MAX_CONSOLE_ERROR_LENGTH
    ? message
    : `${message.slice(0, MAX_CONSOLE_ERROR_LENGTH - 1)}…`;
}

function pollingAllowed(): boolean {
  if (typeof document !== 'undefined' &&
      (document.visibilityState === 'hidden' || document.hidden === true)) {
    return false;
  }
  return typeof navigator === 'undefined' || navigator.onLine !== false;
}

interface StatusPollController {
  onControlSettled: (authoritativeStatusRead: boolean) => void;
  cadenceChanged: () => void;
}

export default function DeviceConsoleApp() {
  const { locale, selectedLocale, localeLoading, localeLoadFailed, setLocale } = useLocale();
  const t = useT();
  const [screen, setScreen] = useState<Screen>('dashboard');
  const [status, setStatus] = useState<DeviceStatus | null>(null);
  const [objects, setObjects] = useState<ObjectStoreState | null>(null);
  const [sessions, setSessions] = useState<RecordingSessionCollection | null>(null);
  const [selectedId, setSelectedId] = useState<string | null>(null);
  /* MAX-167: which recording/session a 3D scan attaches to -- a scan has no
   * valid object-level home any more, so the scanner screen needs both IDs. */
  const [scannerSessionId, setScannerSessionId] = useState<string | null>(null);
  const [linkError, setLinkError] = useState<unknown | null>(null);
  const [workspaceError, setWorkspaceError] = useState<unknown | null>(null);
  const [busy, setBusy] = useState(false);
  const storageUnavailable = status?.storage_available === false;
  const storageUnavailableRef = useRef(storageUnavailable);
  storageUnavailableRef.current = storageUnavailable;
  const previousStorageUnavailableRef = useRef(storageUnavailable);
  const objectRefreshEpoch = useRef(0);
  const sessionRefreshEpoch = useRef(0);
  const objectsRef = useRef<ObjectStoreState | null>(null);
  const reconcileTimer = useRef<number | null>(null);
  const controlsInFlightRef = useRef(0);
  const pollControllerRef = useRef<StatusPollController | null>(null);
  const storageRecoveryOwedRef = useRef(false);
  const refreshWorkspaceAfterStorageRecoveryRef = useRef<() => void>(() => undefined);
  const workflowStepRef = useRef(status?.workflow?.step);
  workflowStepRef.current = status?.workflow?.step;

  const refreshStatus = useCallback(async (): Promise<boolean> => {
    try {
      const next = await fetchDeviceStatus(false);
      setStatus(next);
      setLinkError(null);
      setWorkspaceError((current: unknown) => current instanceof DeviceStatusValidationError ? null : current);
      return true;
    }
    catch (reason) {
      setLinkError(reason);
      return false;
    }
  }, []);

  const refreshObjects = useCallback(async (
    preferredSelectedId: string | null = null,
    selectionMode: ObjectSelectionMode = 'preserve',
  ): Promise<ObjectStoreState | null> => {
    const epoch = ++objectRefreshEpoch.current;
    if (storageUnavailableRef.current) {
      objectsRef.current = null;
      setObjects(null);
      setSessions(null);
      setWorkspaceError(null);
      return null;
    }
    try {
      const next = await fetchDeviceStatus(true);
      if (epoch !== objectRefreshEpoch.current) return null;

      if (next.storage_available === false) {
        setStatus(next);
        objectsRef.current = null;
        setObjects(null);
        setSessions(null);
        setLinkError(null);
        setWorkspaceError(null);
        return null;
      }

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
        ? TranslatedError.fromKey('console.error.object_store_unavailable', {
          detail: nextStore.error ?? TranslatedError.fromKey('objects.storage_error'),
        })
        : null);
      return nextStore?.available === false ? null : nextStore;
    }
    catch (reason) {
      if (epoch !== objectRefreshEpoch.current) return null;
      setWorkspaceError(reason);
      if (reason instanceof DeviceStatusValidationError) setLinkError(reason);
      return null;
    }
  }, []);

  const refreshSessions = useCallback(async (objectId: string | null): Promise<boolean> => {
    const epoch = ++sessionRefreshEpoch.current;
    if (objectId === null) {
      setSessions(null);
      return true;
    }
    if (storageUnavailableRef.current) {
      setSessions(null);
      return false;
    }

    try {
      /* Sessions are an SD model of one object; do not re-read every object profile. */
      const next = await fetchDeviceStatus(false, objectId);
      if (epoch !== sessionRefreshEpoch.current) return true;
      setStatus(next);
      if (next.storage_available === false) {
        objectsRef.current = null;
        setObjects(null);
        setSessions(null);
        setLinkError(null);
        setWorkspaceError(null);
        return false;
      }
      setSessions(next.recording_sessions ?? null);
      setLinkError(null);
      setWorkspaceError((current: unknown) => current instanceof DeviceStatusValidationError ? null : current);
      return next.recording_sessions?.available !== false;
    }
    catch (reason) {
      if (epoch !== sessionRefreshEpoch.current) return false;
      setWorkspaceError(reason);
      if (reason instanceof DeviceStatusValidationError) setLinkError(reason);
      return false;
    }
  }, []);

  refreshWorkspaceAfterStorageRecoveryRef.current = () => {
    void refreshObjects();
    if (screen === 'objects') void refreshSessions(selectedId);
  };

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
   * Poll ownership contract: hidden/offline pauses clear the timer; resume does
   * one immediate read and then restores cadence. A tick during a control is
   * owed once; the final control's successful transition read satisfies it,
   * otherwise one immediate read runs after that control settles.
   */
  useEffect(() => {
    let cancelled = false;
    let timer: number | null = null;
    let failures = 0;
    let readInFlight = false;
    let refreshOwed = false;
    let wasAllowed = pollingAllowed();

    const clearTimer = (): void => {
      if (timer !== null) {
        window.clearTimeout(timer);
        timer = null;
      }
    };

    let poll: () => Promise<void>;
    const schedule = (delay: number): void => {
      if (cancelled || !pollingAllowed()) return;
      clearTimer();
      timer = window.setTimeout(() => {
        timer = null;
        void poll();
      }, delay);
    };

    poll = async (): Promise<void> => {
      if (cancelled || !pollingAllowed()) {
        clearTimer();
        return;
      }
      if (controlsInFlightRef.current > 0) {
        refreshOwed = true;
        schedule(workflowStatusPollDelay(workflowStepRef.current, true, 0));
        return;
      }
      if (readInFlight) return;

      refreshOwed = false;
      readInFlight = true;
      const ok = await refreshStatus();
      readInFlight = false;
      if (cancelled) return;
      failures = ok ? 0 : Math.min(failures + 1, 4);
      if (!pollingAllowed()) return;
      if (controlsInFlightRef.current === 0 && refreshOwed) {
        refreshOwed = false;
        clearTimer();
        void poll();
        return;
      }
      schedule(workflowStatusPollDelay(workflowStepRef.current, ok, failures));
    };

    const onEnvironmentChange = (): void => {
      const allowed = pollingAllowed();
      if (!allowed) {
        wasAllowed = false;
        clearTimer();
        return;
      }
      if (!wasAllowed) {
        wasAllowed = true;
        clearTimer();
        void poll();
      }
    };

    const visibilityDocument = typeof document === 'undefined' ? undefined : document;
    const browserWindow = typeof window === 'undefined' ? undefined : window;
    if (typeof visibilityDocument?.addEventListener === 'function') {
      visibilityDocument.addEventListener('visibilitychange', onEnvironmentChange);
    }
    if (typeof browserWindow?.addEventListener === 'function') {
      browserWindow.addEventListener('online', onEnvironmentChange);
      browserWindow.addEventListener('offline', onEnvironmentChange);
    }

    pollControllerRef.current = {
      onControlSettled: (authoritativeStatusRead) => {
        if (controlsInFlightRef.current > 0) return;
        if (authoritativeStatusRead) {
          refreshOwed = false;
          clearTimer();
          if (!readInFlight && pollingAllowed()) {
            schedule(workflowStatusPollDelay(workflowStepRef.current, true, 0));
          }
          return;
        }
        if (!refreshOwed || !pollingAllowed()) return;
        if (readInFlight) {
          clearTimer();
          return;
        }
        refreshOwed = false;
        clearTimer();
        void poll();
      },
      cadenceChanged: () => {
        failures = 0;
        clearTimer();
        if (!readInFlight && pollingAllowed()) {
          schedule(workflowStatusPollDelay(workflowStepRef.current, true, 0));
        }
      },
    };

    if (wasAllowed) schedule(workflowStatusPollDelay(workflowStepRef.current, true, 0));
    return () => {
      cancelled = true;
      clearTimer();
      if (pollControllerRef.current !== null) pollControllerRef.current = null;
      /* MAX-387 integration point -- when deviceApi exposes a request owner, own the poll reads here and retire the owner on hidden/offline/unmount. */
      if (typeof visibilityDocument?.removeEventListener === 'function') {
        visibilityDocument.removeEventListener('visibilitychange', onEnvironmentChange);
      }
      if (typeof browserWindow?.removeEventListener === 'function') {
        browserWindow.removeEventListener('online', onEnvironmentChange);
        browserWindow.removeEventListener('offline', onEnvironmentChange);
      }
    };
  }, [refreshStatus]);

  const pollCadence = workflowStatusPollDelay(status?.workflow?.step, true, 0);
  const previousPollCadence = useRef(pollCadence);
  useEffect(() => {
    if (previousPollCadence.current === pollCadence) return;
    previousPollCadence.current = pollCadence;
    pollControllerRef.current?.cadenceChanged();
  }, [pollCadence]);

  /* Populate Home/Objects from one extended request. */
  useEffect(() => {
    void refreshObjects();
  }, [refreshObjects]);

  useEffect(() => {
    const wasUnavailable = previousStorageUnavailableRef.current;
    previousStorageUnavailableRef.current = storageUnavailable;
    if (wasUnavailable === storageUnavailable) return;
    if (storageUnavailable) {
      objectRefreshEpoch.current += 1;
      sessionRefreshEpoch.current += 1;
      objectsRef.current = null;
      setObjects(null);
      setSessions(null);
      setWorkspaceError(null);
      return;
    }
    /* Refill the object cache once after recovery instead of replaying every screen effect. */
    if (controlsInFlightRef.current > 0) {
      storageRecoveryOwedRef.current = true;
      return;
    }
    refreshWorkspaceAfterStorageRecoveryRef.current();
  }, [refreshSessions, refreshObjects, screen, selectedId, storageUnavailable]);

  /*
   * Reconcile the complete object database infrequently. Confirmed web writes
   * are projected into React state immediately; this slow read is only for
   * physical-UI/other-client changes and recovery from an ambiguous transport
   * failure. Crucially, changing selectedId no longer triggers a full SD scan.
   */
  useEffect(() => {
    if (screen !== 'objects') return undefined;

    void refreshObjects();
    const slow = window.setInterval(() => {
      if (pollingAllowed() && controlsInFlightRef.current === 0) void refreshObjects();
    }, 60000);
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
    let authoritativeStatusRead = false;
    const readTransitionStatus = async (): Promise<DeviceStatus> => {
      const next = await fetchDeviceStatus(false);
      authoritativeStatusRead = true;
      return next;
    };

    controlsInFlightRef.current += 1;
    setBusy(true);
    try {
      return await runAfterControlConfirmation(sendControl(action, payload), async (result) => {
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
          readTransitionStatus,
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
            readTransitionStatus,
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
          throw TranslatedError.fromKey('console.error.transition_failed', {
            action,
            transitionError: new Error(transitionError),
          });
        }
      }
      else if (!LOCAL_OBJECT_MUTATION_ACTIONS.has(action)) {
        authoritativeStatusRead = await refreshStatus();
      }

      return true;
      });
    }
    catch (reason) {
      const rawMessage = reason instanceof Error ? reason.message : String(reason);

      /*
       * Never replay a write whose reply was lost: it might already be durable
       * on SD. A delayed authoritative GET is safe and, unlike the old code,
       * does not block the button handler or race the fast status poll.
       */
      const storageAvailabilityFailure = isStorageAvailabilityError(reason);
      if (!storageAvailabilityFailure && transportFailure(rawMessage) && LOCAL_OBJECT_MUTATION_ACTIONS.has(action)) {
        scheduleObjectReconcile(selectionMode);
      }
      else if (!storageAvailabilityFailure && action === 'measurement_start') {
        /* Reconcile the authoritative selection after a rejected or failed
         * async START; admission itself is reject-safe before activation. */
        scheduleObjectReconcile('active');
      }

      setWorkspaceError(reason);
      if (transportFailure(rawMessage)) setLinkError(reason);
      return false;
    }
    finally {
      controlsInFlightRef.current = Math.max(0, controlsInFlightRef.current - 1);
      if (controlsInFlightRef.current === 0) {
        if (storageRecoveryOwedRef.current) {
          storageRecoveryOwedRef.current = false;
          refreshWorkspaceAfterStorageRecoveryRef.current();
        }
        pollControllerRef.current?.onControlSettled(authoritativeStatusRead);
      }
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
        <button className="console-floating-back" onClick={() => setScreen('objects')}>{t('console.back_object')}</button>
        <StorageUnavailableBanner status={status} />
        <ScannerFeatureHost
          objectId={scannerObjectId}
          sessionId={scannerSessionId}
          status={status}
          onBack={() => setScreen('objects')}
          onDashboard={() => setScreen('dashboard')}
        />
      </div>
    );
  }

  const apiOnline = status !== null && linkError === null;
  const apiLabel = status === null && linkError === null ? t('console.api_connecting') : apiOnline ? t('console.api_link') : t('console.api_error');
  const pageErrorReason = workspaceError ?? linkError;
  const pageError = pageErrorReason === null ? null : boundErrorText(formatErrorText(locale, pageErrorReason));

  return (
    <main className="device-console-shell">
      <header className="console-topbar">
        <button className="brand-button" onClick={() => setScreen('dashboard')}>
          <span>GEOLOCATOR</span>
          <small>{selectedObject?.name ?? 'ESP32-S3'}</small>
        </button>
        <nav>
          <button aria-pressed={screen === 'dashboard'} onClick={() => setScreen('dashboard')}>{t('nav.home')}</button>
          <button aria-pressed={screen === 'objects'} onClick={() => setScreen('objects')}>{t('nav.objects')}</button>
          <button aria-pressed={screen === 'sensors'} onClick={() => setScreen('sensors')}>{t('nav.sensors')}</button>
          <button aria-pressed={screen === 'workflow'} onClick={() => setScreen('workflow')}>{t('workflow.nav')}</button>
        </nav>
        <div className="console-topbar-actions">
          <label className="locale-picker">
            <span className="sr-only">{t('web.locale_label')}</span>
            {/* The selector shows the requested locale; content stays EN until UK is ready. */}
            <select aria-label={t('web.locale_label')} aria-busy={localeLoading} value={selectedLocale} onChange={(event) => setLocale(event.target.value as 'en' | 'uk')}>
              <option value="en">EN</option>
              <option value="uk">UK</option>
            </select>
          </label>
          {(localeLoading || localeLoadFailed) && (
            <small role="status" aria-live="polite">
              {localeLoading ? t('web.locale_loading') : t('web.locale_load_failed')}
            </small>
          )}
          <div className={`device-dot ${apiOnline ? 'online' : 'offline'}`}>{apiLabel}</div>
        </div>
      </header>

      <PowerBanner status={status} />
      <StorageUnavailableBanner status={status} />

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
            if (!saved) throw TranslatedError.fromKey('console.error.phone_gps_not_saved');
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
          onStartWorkflow={(id) => { setSelectedId(id); setScreen('workflow'); }}
          onOpenScanner={(id, sessionId) => { setSelectedId(id); setScannerSessionId(sessionId); setScreen('scanner'); }}
        />
      )}

      {screen === 'sensors' && (
        <SensorsFeature
          status={status}
          object={selectedObject}
          error={pageError}
          onCalibrateHeights={() => runControl('calibrate_heights')}
        />
      )}

      {screen === 'workflow' && (
        <WorkflowPanel
          status={status}
          objectName={selectedObject?.name ?? null}
          sendCommand={sendWorkflowCommand}
          onRefreshStatus={refreshStatus}
          onWorkflowUpdate={(workflow) => {
            setStatus((current) => current === null ? current : { ...current, workflow });
          }}
          onOpenScanner={(id, sessionId) => { setSelectedId(id); setScannerSessionId(sessionId); setScreen('scanner'); }}
        />
      )}
    </main>
  );
}
