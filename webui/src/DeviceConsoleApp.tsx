import { lazy, Suspense, useCallback, useEffect, useMemo, useState } from 'react';
import './console.css';
import { capturePhoneLocation, fetchDeviceStatus, sendControl } from './api/deviceApi';
import type {
  DeviceStatus,
  ObjectStoreState,
  PhoneLocationInput,
  RecordingSessionCollection,
} from './domain/device';
import DeviceDashboard from './features/dashboard/DeviceDashboard';

const ObjectsFeature = lazy(() => import('./features/objects/ObjectsFeature'));
const SensorsFeature = lazy(() => import('./features/sensors/SensorsFeature'));
const XrScannerFeature = lazy(() => import('./App'));

type Screen = 'dashboard' | 'objects' | 'sensors' | 'scanner';

export default function DeviceConsoleApp() {
  const [screen, setScreen] = useState<Screen>('dashboard');
  const [status, setStatus] = useState<DeviceStatus | null>(null);
  const [objects, setObjects] = useState<ObjectStoreState | null>(null);
  const [sessions, setSessions] = useState<RecordingSessionCollection | null>(null);
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const refreshStatus = useCallback(async () => {
    try {
      const next = await fetchDeviceStatus(false);
      setStatus(next);
      setError(null);
    }
    catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    }
  }, []);

  const refreshObjects = useCallback(async () => {
    try {
      const next = await fetchDeviceStatus(true, selectedId);
      setStatus(next);
      setObjects(next.object_store ?? null);
      setSessions(next.recording_sessions ?? null);
      setSelectedId((current) => current ?? next.object_store?.active_id ?? next.object_store?.objects[0]?.id ?? null);
      setError(null);
    }
    catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    }
  }, [selectedId]);

  useEffect(() => {
    void refreshStatus();
    void refreshObjects();
    const fast = window.setInterval(() => void refreshStatus(), 2000);
    const slow = window.setInterval(() => void refreshObjects(), 8000);
    return () => {
      window.clearInterval(fast);
      window.clearInterval(slow);
    };
  }, [refreshObjects, refreshStatus]);

  const runControl = useCallback(async (action: string, payload: Record<string, unknown> = {}) => {
    setBusy(true);
    try {
      await sendControl(action, payload);
      await refreshObjects();
      setError(null);
    }
    catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
      throw reason;
    }
    finally {
      setBusy(false);
    }
  }, [refreshObjects]);

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
        <div className={`device-dot ${error === null ? 'online' : 'offline'}`}>{error === null ? 'ESP LINK' : 'API ERROR'}</div>
      </header>

      {screen === 'dashboard' && (
        <DeviceDashboard
          status={status}
          objects={objects}
          error={error}
          onOpenObjects={() => setScreen('objects')}
          onOpenSensors={() => setScreen('sensors')}
          onOpenActiveObject={openActive}
          onRefresh={() => { void refreshStatus(); void refreshObjects(); }}
        />
      )}

      {screen === 'objects' && (
        <Suspense fallback={<div className="console-loading">Loading object workspace…</div>}>
          <ObjectsFeature
            status={status}
            store={objects}
            sessions={sessions}
            selectedId={selectedId}
            busy={busy}
            error={error}
            onSelect={setSelectedId}
            onCreate={(name) => runControl('create_object', name ? { name } : {})}
            onActivate={(id) => runControl('activate_object', { object_id: id })}
            onRename={(id, name) => runControl('rename_object', { object_id: id, name })}
            onDelete={async (id) => { await runControl('delete_object', { object_id: id }); setSelectedId(null); }}
            onBindGps={async (id): Promise<PhoneLocationInput> => {
              const position = await capturePhoneLocation();
              await runControl('set_object_location', { object_id: id, ...position });
              return position;
            }}
            onCable={(id, podId, length) => runControl('set_object_cable_length', { object_id: id, pod_id: podId, length_m: length })}
            onDefaultCable={(podId, length) => runControl('set_default_cable_length', { pod_id: podId, length_m: length })}
            onAcquisition={(running) => runControl(running ? 'acquisition_start' : 'acquisition_stop')}
            onRecording={(id, running) => runControl(running ? 'recording_start' : 'recording_stop', running ? { object_id: id } : {})}
            onOpenScanner={(id) => { setSelectedId(id); setScreen('scanner'); }}
          />
        </Suspense>
      )}

      {screen === 'sensors' && (
        <Suspense fallback={<div className="console-loading">Loading sensor diagnostics…</div>}>
          <SensorsFeature status={status} object={selectedObject} />
        </Suspense>
      )}
    </main>
  );
}
