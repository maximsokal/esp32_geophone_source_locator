import { useEffect, useMemo, useState } from 'react';

import {
  probeWebXrCapabilities,
  WebXrRuntime,
  type WebXrCapabilities,
  type WebXrRuntimeState,
} from '../xr/webxrRuntime';

const emptyRuntimeState: WebXrRuntimeState = {
  phase: 'idle',
  referenceSpace: null,
  domOverlay: false,
  environmentBlendMode: null,
  frameCount: 0,
  tracking: false,
  pose: null,
  error: null,
};

function capabilityClass(value: boolean): string {
  return value ? 'xr-capability good' : 'xr-capability bad';
}

function capabilityText(value: boolean): string {
  return value ? 'PASS' : 'NO';
}

function formatPosition(value: readonly [number, number, number] | null): string {
  if (value === null) {
    return '--';
  }

  return value.map((component) => component.toFixed(3)).join(' / ');
}

function formatOrientation(value: readonly [number, number, number, number] | null): string {
  if (value === null) {
    return '--';
  }

  return value.map((component) => component.toFixed(3)).join(' / ');
}

function phaseLabel(state: WebXrRuntimeState): string {
  switch (state.phase) {
    case 'starting':
      return 'STARTING';
    case 'running':
      return state.tracking ? 'TRACKING' : 'WAITING FOR POSE';
    case 'stopping':
      return 'STOPPING';
    case 'ended':
      return 'ENDED';
    case 'error':
      return 'ERROR';
    case 'idle':
    default:
      return 'IDLE';
  }
}

export function XrRuntimePanel() {
  const runtime = useMemo(() => new WebXrRuntime(), []);
  const [capabilities, setCapabilities] = useState<WebXrCapabilities | null>(null);
  const [runtimeState, setRuntimeState] = useState<WebXrRuntimeState>(emptyRuntimeState);

  useEffect(() => {
    const unsubscribe = runtime.subscribe(setRuntimeState);
    let cancelled = false;

    void probeWebXrCapabilities().then((result) => {
      if (!cancelled) {
        setCapabilities(result);
      }
    });

    return () => {
      cancelled = true;
      unsubscribe();
      void runtime.dispose();
    };
  }, [runtime]);

  const running = runtimeState.phase === 'running';
  const busy = runtimeState.phase === 'starting' || runtimeState.phase === 'stopping';
  const canStart =
    capabilities?.secureContext === true &&
    capabilities.webGl === true &&
    capabilities.xrApi === true &&
    capabilities.immersiveAr === true &&
    !running &&
    !busy;

  const posePosition = runtimeState.pose?.position ?? null;
  const poseOrientation = runtimeState.pose?.orientation ?? null;

  return (
    <>
      <section className="xr-runtime-card" aria-label="WebXR runtime diagnostics">
        <div className="xr-runtime-heading">
          <div>
            <div className="panel-title">WEB-XR-A · browser AR runtime</div>
            <strong>Immersive AR capability & tracking</strong>
          </div>
          <span className={`xr-phase ${runtimeState.phase === 'error' ? 'bad' : running ? 'good' : ''}`}>
            {phaseLabel(runtimeState)}
          </span>
        </div>

        <div className="xr-capability-grid">
          <div className={capabilityClass(capabilities?.secureContext === true)}>
            <span>Trusted HTTPS</span>
            <strong>{capabilities === null ? '...' : capabilityText(capabilities.secureContext)}</strong>
          </div>
          <div className={capabilityClass(capabilities?.cameraApi === true)}>
            <span>Camera API</span>
            <strong>{capabilities === null ? '...' : capabilityText(capabilities.cameraApi)}</strong>
          </div>
          <div className={capabilityClass(capabilities?.webGl === true)}>
            <span>WebGL</span>
            <strong>{capabilities === null ? '...' : capabilityText(capabilities.webGl)}</strong>
          </div>
          <div className={capabilityClass(capabilities?.xrApi === true)}>
            <span>WebXR API</span>
            <strong>{capabilities === null ? '...' : capabilityText(capabilities.xrApi)}</strong>
          </div>
          <div className={capabilityClass(capabilities?.immersiveAr === true)}>
            <span>immersive-ar</span>
            <strong>{capabilities === null ? '...' : capabilityText(capabilities.immersiveAr)}</strong>
          </div>
          <div className="xr-capability neutral">
            <span>Depth sensing</span>
            <strong>NEXT STAGE</strong>
          </div>
        </div>

        <div className="xr-runtime-controls">
          <button
            type="button"
            className="xr-start-button"
            disabled={!canStart}
            onClick={() => void runtime.start()}
          >
            {runtimeState.phase === 'starting' ? 'Starting AR…' : 'Start AR session'}
          </button>

          <button
            type="button"
            className="xr-stop-button"
            disabled={!running && runtimeState.phase !== 'stopping'}
            onClick={() => void runtime.stop()}
          >
            End AR session
          </button>

          <span className="xr-runtime-note">
            Uses a transparent XRWebGLLayer and tracks the phone pose in local-floor, with local fallback.
          </span>
        </div>

        <dl className="xr-runtime-values">
          <div>
            <dt>Reference space</dt>
            <dd>{runtimeState.referenceSpace ?? '--'}</dd>
          </div>
          <div>
            <dt>DOM overlay</dt>
            <dd>{running ? (runtimeState.domOverlay ? 'AVAILABLE' : 'NOT AVAILABLE') : '--'}</dd>
          </div>
          <div>
            <dt>Environment blend</dt>
            <dd>{runtimeState.environmentBlendMode ?? '--'}</dd>
          </div>
          <div>
            <dt>Frames</dt>
            <dd>{runtimeState.frameCount.toLocaleString()}</dd>
          </div>
          <div>
            <dt>Tracking</dt>
            <dd>{running ? (runtimeState.tracking ? 'POSE VALID' : 'WAITING') : '--'}</dd>
          </div>
          <div>
            <dt>Views</dt>
            <dd>{runtimeState.pose?.views ?? '--'}</dd>
          </div>
          <div className="wide">
            <dt>Phone XYZ, m</dt>
            <dd>{formatPosition(posePosition)}</dd>
          </div>
          <div className="wide">
            <dt>Quaternion XYZW</dt>
            <dd>{formatOrientation(poseOrientation)}</dd>
          </div>
        </dl>

        {runtimeState.error !== null && (
          <div className="xr-runtime-error" role="alert">
            {runtimeState.error}
          </div>
        )}
      </section>

      <div className="xr-immersive-overlay" aria-live="polite">
        <div className="xr-overlay-card">
          <div className="eyebrow">GEOLOCATOR · IMMERSIVE AR</div>
          <h2>{runtimeState.tracking ? 'Phone tracking active' : 'Waiting for AR tracking…'}</h2>
          <p>
            Reference: <b>{runtimeState.referenceSpace ?? '--'}</b> · Frames:{' '}
            <b>{runtimeState.frameCount.toLocaleString()}</b>
          </p>
          <div className="xr-overlay-pose">
            <span>XYZ m</span>
            <strong>{formatPosition(posePosition)}</strong>
          </div>
          <button type="button" onClick={() => void runtime.stop()}>
            End AR session
          </button>
        </div>
      </div>
    </>
  );
}
