import { useEffect, useMemo, useState } from 'react';

import {
  probeWebXrCapabilities,
  WebXrRuntime,
  type ScanPointCloudSnapshot,
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
  depthAvailable: false,
  depthUsage: null,
  depthDataFormat: null,
  depthWidth: null,
  depthHeight: null,
  depthFrameCount: 0,
  sampledDepthFrames: 0,
  pointCount: 0,
  pointCloudVersion: 0,
  depthError: null,
  error: null,
};

interface XrRuntimePanelProps {
  onSessionActiveChange?: (active: boolean) => void;
  onPointCloudReady?: (snapshot: ScanPointCloudSnapshot | null) => void;
}

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
      return 'STARTING SCAN';
    case 'running':
      if (!state.tracking) {
        return 'WAITING FOR POSE';
      }
      return state.depthAvailable ? 'SCANNING 3D' : 'TRACKING ONLY';
    case 'stopping':
      return 'FINISHING';
    case 'ended':
      return state.pointCount > 0 ? '3D SCAN READY' : 'ENDED';
    case 'error':
      return 'ERROR';
    case 'idle':
    default:
      return 'IDLE';
  }
}

function depthLabel(state: WebXrRuntimeState): string {
  if (state.phase === 'starting') {
    return 'REQUESTING';
  }

  if (state.phase === 'running' || state.phase === 'stopping') {
    return state.depthAvailable ? 'ACTIVE' : 'UNAVAILABLE';
  }

  if (state.phase === 'ended') {
    if (state.pointCount > 0) {
      return 'CAPTURED';
    }
    return state.depthAvailable ? 'NO POINTS' : 'UNAVAILABLE';
  }

  return 'CHECK ON START';
}

function depthCapabilityClass(state: WebXrRuntimeState): string {
  if (state.phase === 'idle') {
    return 'xr-capability neutral';
  }

  if (state.phase === 'starting') {
    return 'xr-capability neutral';
  }

  return state.depthAvailable || state.pointCount > 0
    ? 'xr-capability good'
    : 'xr-capability bad';
}

export function XrRuntimePanel({
  onSessionActiveChange,
  onPointCloudReady,
}: XrRuntimePanelProps) {
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

  useEffect(() => {
    const active =
      runtimeState.phase === 'starting' ||
      runtimeState.phase === 'running' ||
      runtimeState.phase === 'stopping';

    onSessionActiveChange?.(active);
  }, [onSessionActiveChange, runtimeState.phase]);

  useEffect(() => {
    if (runtimeState.phase === 'starting') {
      onPointCloudReady?.(null);
      return;
    }

    if (runtimeState.phase === 'ended' && runtimeState.pointCount > 0) {
      onPointCloudReady?.(runtime.getPointCloudSnapshot());
    }
  }, [onPointCloudReady, runtime, runtimeState.phase, runtimeState.pointCount]);

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
  const depthSize =
    runtimeState.depthWidth !== null && runtimeState.depthHeight !== null
      ? `${runtimeState.depthWidth} × ${runtimeState.depthHeight}`
      : '--';

  return (
    <>
      <section className="xr-runtime-card" aria-label="WebXR depth scanner">
        <div className="xr-runtime-heading">
          <div>
            <div className="panel-title">WEB-XR-B/C · metric browser 3D scanner</div>
            <strong>ARCore depth → world-space point cloud</strong>
          </div>
          <span className={`xr-phase ${runtimeState.phase === 'error' ? 'bad' : running || runtimeState.pointCount > 0 ? 'good' : ''}`}>
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
          <div className={depthCapabilityClass(runtimeState)}>
            <span>Depth sensing</span>
            <strong>{depthLabel(runtimeState)}</strong>
          </div>
        </div>

        <div className="xr-runtime-controls">
          <button
            type="button"
            className="xr-start-button"
            disabled={!canStart}
            onClick={() => void runtime.start()}
          >
            {runtimeState.phase === 'starting' ? 'Starting scanner…' : 'Start 3D scan'}
          </button>

          <button
            type="button"
            className="xr-stop-button"
            disabled={!running && runtimeState.phase !== 'stopping'}
            onClick={() => void runtime.stop()}
          >
            Finish scan & build 3D
          </button>

          <span className="xr-runtime-note">
            The phone stays local: WebXR depth samples are converted to metric XYZ and voxel-decimated in browser memory. No photo upload is required.
          </span>
        </div>

        <dl className="xr-runtime-values">
          <div>
            <dt>Reference</dt>
            <dd>{runtimeState.referenceSpace ?? '--'}</dd>
          </div>
          <div>
            <dt>Depth mode</dt>
            <dd>{runtimeState.depthUsage ?? '--'}</dd>
          </div>
          <div>
            <dt>Depth format</dt>
            <dd>{runtimeState.depthDataFormat ?? '--'}</dd>
          </div>
          <div>
            <dt>Depth buffer</dt>
            <dd>{depthSize}</dd>
          </div>
          <div>
            <dt>Depth frames</dt>
            <dd>{runtimeState.depthFrameCount.toLocaleString()}</dd>
          </div>
          <div>
            <dt>3D points</dt>
            <dd>{runtimeState.pointCount.toLocaleString()}</dd>
          </div>
          <div>
            <dt>XR frames</dt>
            <dd>{runtimeState.frameCount.toLocaleString()}</dd>
          </div>
          <div>
            <dt>Sampled frames</dt>
            <dd>{runtimeState.sampledDepthFrames.toLocaleString()}</dd>
          </div>
          <div>
            <dt>Tracking</dt>
            <dd>{running ? (runtimeState.tracking ? 'POSE VALID' : 'WAITING') : '--'}</dd>
          </div>
          <div>
            <dt>DOM overlay</dt>
            <dd>{running ? (runtimeState.domOverlay ? 'AVAILABLE' : 'NO') : '--'}</dd>
          </div>
          <div>
            <dt>Blend</dt>
            <dd>{runtimeState.environmentBlendMode ?? '--'}</dd>
          </div>
          <div>
            <dt>Views</dt>
            <dd>{runtimeState.pose?.views ?? '--'}</dd>
          </div>
          <div className="wide">
            <dt>Phone XYZ, m (WebXR)</dt>
            <dd>{formatPosition(posePosition)}</dd>
          </div>
          <div className="wide">
            <dt>Quaternion XYZW</dt>
            <dd>{formatOrientation(poseOrientation)}</dd>
          </div>
        </dl>

        {runtimeState.depthError !== null && (
          <div className="xr-runtime-warning" role="status">
            {runtimeState.depthError}
          </div>
        )}

        {runtimeState.error !== null && (
          <div className="xr-runtime-error" role="alert">
            {runtimeState.error}
          </div>
        )}
      </section>

      <div className="xr-immersive-overlay" aria-live="polite">
        <div className="xr-overlay-card">
          <div className="eyebrow">GEOLOCATOR · LIVE METRIC 3D SCAN</div>
          <h2>
            {!runtimeState.tracking
              ? 'Waiting for AR tracking…'
              : runtimeState.depthAvailable
                ? 'Depth processing active'
                : 'Tracking works, depth unavailable'}
          </h2>
          <p>
            Reference: <b>{runtimeState.referenceSpace ?? '--'}</b> · XR frames:{' '}
            <b>{runtimeState.frameCount.toLocaleString()}</b>
          </p>

          <div className="xr-overlay-scan-grid">
            <div>
              <span>DEPTH</span>
              <strong className={runtimeState.depthAvailable ? 'good-text' : 'bad-text'}>
                {runtimeState.depthAvailable ? 'ACTIVE' : 'UNAVAILABLE'}
              </strong>
            </div>
            <div>
              <span>DEPTH FRAMES</span>
              <strong>{runtimeState.depthFrameCount.toLocaleString()}</strong>
            </div>
            <div>
              <span>SAMPLED</span>
              <strong>{runtimeState.sampledDepthFrames.toLocaleString()}</strong>
            </div>
            <div>
              <span>3D POINTS</span>
              <strong>{runtimeState.pointCount.toLocaleString()}</strong>
            </div>
          </div>

          <div className="xr-overlay-pose">
            <span>Phone XYZ m</span>
            <strong>{formatPosition(posePosition)}</strong>
          </div>

          {runtimeState.depthError !== null && (
            <div className="xr-overlay-warning">{runtimeState.depthError}</div>
          )}

          <button type="button" onClick={() => void runtime.stop()}>
            Finish scan & view 3D
          </button>
        </div>
      </div>
    </>
  );
}
