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
  scanPaused: false,
  wakeLockActive: false,
  wakeLockError: null,
  depthAvailable: false,
  depthUsage: null,
  depthDataFormat: null,
  depthType: null,
  depthWidth: null,
  depthHeight: null,
  depthFrameCount: 0,
  sampledDepthFrames: 0,
  pointCount: 0,
  retainedPointCount: 0,
  pointCloudVersion: 0,
  keyframeCount: 0,
  polishFrameCount: 0,
  motionRejectedFrames: 0,
  motionState: null,
  motionQuality: 0,
  linearSpeedMetersPerSecond: 0,
  angularSpeedRadiansPerSecond: 0,
  linearAccelerationMetersPerSecond2: 0,
  coverageGuideActive: false,
  coverageGuideRenderPoints: 0,
  coverageVisibleCells: 0,
  coverageReadyCells: 0,
  coverageTotalCells: 0,
  coverageTargetIndependentViews: 3,
  coverageAverageConfidence: 0,
  coverageGuideError: null,
  lightingEstimateAvailable: false,
  primaryLightIntensity: null,
  depthError: null,
  error: null,
};

interface XrRuntimePanelProps {
  onSessionActiveChange?: (active: boolean) => void;
  onPointCloudReady?: (snapshot: ScanPointCloudSnapshot | null) => void;
}

function phaseLabel(state: WebXrRuntimeState): string {
  if (state.phase === 'starting') return 'STARTING';
  if (state.phase === 'stopping') return 'FINISHING';
  if (state.phase === 'interrupted') return 'SCAN INTERRUPTED';
  if (state.phase === 'ended') return state.pointCount > 0 ? 'SCAN READY' : 'NO STABLE SURFACE';
  if (state.phase === 'error') return 'ERROR';
  if (state.phase !== 'running') return 'READY';
  if (state.scanPaused) return 'PAUSED';
  if (!state.tracking) return 'FINDING TRACKING';
  if (!state.depthAvailable) return 'DEPTH UNAVAILABLE';
  if (state.motionState === 'too-fast') return 'MOVE SLOWER';
  if (state.motionState === 'stationary' || state.motionState === 'redundant') {
    return 'MOVE TO A NEW VIEW';
  }
  if (!state.coverageGuideActive) return 'BUILDING SURFACE';
  return 'SCANNING';
}

function qualityClass(state: WebXrRuntimeState): string {
  if (state.phase === 'error' || state.phase === 'interrupted') return 'bad';
  if (state.phase === 'running' && state.motionState === 'too-fast') return 'warning';
  if (state.phase === 'running' || state.phase === 'ended') return 'good';
  return '';
}

function formatPercent(value: number): string {
  return `${Math.round(Math.max(0, Math.min(1, value)) * 100)}%`;
}

function formatMotion(state: WebXrRuntimeState): string {
  if (state.scanPaused) return 'PAUSED';
  switch (state.motionState) {
    case 'keyframe': return 'NEW VIEW ✓';
    case 'polish': return 'POLISHING';
    case 'stationary': return 'MOVE PHONE';
    case 'redundant': return 'NEW ANGLE';
    case 'too-fast': return 'TOO FAST';
    case 'initializing': return 'INITIALIZING';
    default: return '--';
  }
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
      if (!cancelled) setCapabilities(result);
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
    } else if (runtimeState.phase === 'ended' && runtimeState.pointCount > 0) {
      onPointCloudReady?.(runtime.getPointCloudSnapshot());
    }
    // An unexpected XR end is intentionally NOT finalized as a valid scan.
  }, [onPointCloudReady, runtime, runtimeState.phase, runtimeState.pointCount]);

  const running = runtimeState.phase === 'running';
  const busy = runtimeState.phase === 'starting' || runtimeState.phase === 'stopping';
  const canStart =
    capabilities?.secureContext === true &&
    capabilities.webGl === true &&
    capabilities.xrApi === true &&
    capabilities.immersiveAr === true &&
    !running && !busy;
  const readyVisible = `${runtimeState.coverageReadyCells.toLocaleString()} / ${runtimeState.coverageVisibleCells.toLocaleString()}`;

  return (
    <>
      <section className="xr-runtime-card xr-runtime-card-compact" aria-label="3D scanner">
        <div className="xr-runtime-heading">
          <div>
            <div className="panel-title">3D SCANNER</div>
            <strong>ARCore depth · robust local fusion</strong>
          </div>
          <span className={`xr-phase ${qualityClass(runtimeState)}`}>{phaseLabel(runtimeState)}</span>
        </div>

        <div className="xr-compact-status">
          <span className={capabilities?.immersiveAr ? 'good-text' : 'bad-text'}>
            WebXR {capabilities?.immersiveAr ? '✓' : '—'}
          </span>
          <span className={runtimeState.depthAvailable ? 'good-text' : ''}>
            Depth {runtimeState.depthAvailable ? '✓' : runtimeState.phase === 'idle' ? 'on start' : '—'}
          </span>
          <span className={runtimeState.wakeLockActive ? 'good-text' : ''}>
            Screen {runtimeState.wakeLockActive ? 'awake ✓' : capabilities?.wakeLockApi ? 'wake-ready' : 'wake-lock —'}
          </span>
        </div>

        <div className="xr-runtime-controls xr-runtime-controls-compact">
          <button
            type="button"
            className="xr-start-button"
            disabled={!canStart}
            onClick={() => void runtime.start()}
          >
            {runtimeState.phase === 'interrupted' ? 'Discard partial & start again' : 'Start 3D scan'}
          </button>
          <button
            type="button"
            className="xr-stop-button"
            disabled={!running}
            onClick={() => void runtime.stop()}
          >
            Finish scan
          </button>
        </div>

        {runtimeState.phase === 'interrupted' && (
          <div className="xr-runtime-warning" role="status">
            XR session ended unexpectedly. Partial geometry is retained in memory but was not marked READY because a new XR session may use a different reference frame.
          </div>
        )}

        {runtimeState.depthError !== null && (
          <div className="xr-runtime-warning" role="status">{runtimeState.depthError}</div>
        )}
        {runtimeState.error !== null && (
          <div className="xr-runtime-error" role="alert">{runtimeState.error}</div>
        )}

        <details className="xr-technical-details">
          <summary>Technical details</summary>
          <dl className="xr-runtime-values">
            <div><dt>Depth type</dt><dd>{runtimeState.depthType ?? '--'}</dd></div>
            <div><dt>Depth format</dt><dd>{runtimeState.depthDataFormat ?? '--'}</dd></div>
            <div><dt>Depth size</dt><dd>{runtimeState.depthWidth && runtimeState.depthHeight ? `${runtimeState.depthWidth} × ${runtimeState.depthHeight}` : '--'}</dd></div>
            <div><dt>Stable points</dt><dd>{runtimeState.pointCount.toLocaleString()}</dd></div>
            <div><dt>Retained hypotheses</dt><dd>{runtimeState.retainedPointCount.toLocaleString()}</dd></div>
            <div><dt>Keyframes</dt><dd>{runtimeState.keyframeCount.toLocaleString()}</dd></div>
            <div><dt>Polish frames</dt><dd>{runtimeState.polishFrameCount.toLocaleString()}</dd></div>
            <div><dt>Motion rejects</dt><dd>{runtimeState.motionRejectedFrames.toLocaleString()}</dd></div>
            <div><dt>Ready / visible cells</dt><dd>{readyVisible}</dd></div>
            <div><dt>Coverage confidence</dt><dd>{formatPercent(runtimeState.coverageAverageConfidence)}</dd></div>
            <div><dt>Linear speed</dt><dd>{runtimeState.linearSpeedMetersPerSecond.toFixed(2)} m/s</dd></div>
            <div><dt>Angular speed</dt><dd>{runtimeState.angularSpeedRadiansPerSecond.toFixed(2)} rad/s</dd></div>
            <div><dt>Acceleration</dt><dd>{runtimeState.linearAccelerationMetersPerSecond2.toFixed(2)} m/s²</dd></div>
            <div><dt>Motion quality</dt><dd>{formatPercent(runtimeState.motionQuality)}</dd></div>
            <div><dt>Lighting estimate</dt><dd>{runtimeState.lightingEstimateAvailable ? 'AVAILABLE' : '--'}</dd></div>
            <div><dt>Primary light magnitude</dt><dd>{runtimeState.primaryLightIntensity?.toFixed(3) ?? '--'}</dd></div>
            <div><dt>Wake lock</dt><dd>{runtimeState.wakeLockActive ? 'ACTIVE' : runtimeState.wakeLockError ?? '--'}</dd></div>
          </dl>
        </details>
      </section>

      <div className="xr-immersive-overlay" aria-live="polite">
        <div className="xr-overlay-card xr-overlay-card-compact">
          <div className="xr-overlay-topline">
            <strong className={runtimeState.motionState === 'too-fast' ? 'bad-text' : ''}>
              {phaseLabel(runtimeState)}
            </strong>
            <span>{runtimeState.wakeLockActive ? 'SCREEN LOCK ✓' : ''}</span>
          </div>

          <div className="xr-overlay-scan-grid xr-overlay-scan-grid-compact">
            <div><span>STABLE</span><strong>{runtimeState.pointCount.toLocaleString()}</strong></div>
            <div><span>NEW VIEWS</span><strong>{runtimeState.keyframeCount.toLocaleString()}</strong></div>
            <div><span>READY / SEEN</span><strong>{readyVisible}</strong></div>
            <div><span>MOTION</span><strong>{formatMotion(runtimeState)}</strong></div>
          </div>

          {runtimeState.coverageGuideActive && (
            <div className="xr-coverage-legend" aria-label="Coverage quality">
              <span><i className="coverage-dot coverage-low" />weak</span>
              <span><i className="coverage-dot coverage-mid" />improving</span>
              <span><i className="coverage-dot coverage-high" />multi-view stable</span>
            </div>
          )}

          {runtimeState.scanPaused && (
            <div className="xr-overlay-warning">Scan paused while XR is hidden. It will continue if this XR session becomes visible again.</div>
          )}
          {runtimeState.motionState === 'too-fast' && (
            <div className="xr-overlay-warning">Move slower — this depth sample is not being fused.</div>
          )}
          {(runtimeState.motionState === 'stationary' || runtimeState.motionState === 'redundant') && (
            <div className="xr-overlay-hint">Move 5–10 cm and change viewpoint. Standing still can polish geometry but cannot increase coverage confidence.</div>
          )}

          <button type="button" onClick={() => void runtime.stop()}>Finish scan</button>
        </div>
      </div>
    </>
  );
}
