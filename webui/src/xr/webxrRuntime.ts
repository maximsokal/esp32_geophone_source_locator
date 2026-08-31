import {
  DepthPointCloudAccumulator,
  type CpuDepthInformationLike,
  type DepthFusionFrameContext,
  type DepthSampleView,
  type LiveCoverageSnapshot,
  type ScanPointCloudSnapshot,
} from './depthPointCloud';
import {
  LiveCoveragePointRenderer,
  type CoverageRenderView,
  type CoverageRenderViewport,
} from './liveCoverageRenderer';
import {
  WebXrFusionKeyframeSelector,
  type FusionMotionState,
} from './webxrFusionQuality';

export type XrRuntimePhase =
  | 'idle'
  | 'starting'
  | 'running'
  | 'stopping'
  | 'ended'
  | 'interrupted'
  | 'error';

export interface WebXrCapabilities {
  secureContext: boolean;
  cameraApi: boolean;
  webGl: boolean;
  xrApi: boolean;
  immersiveAr: boolean;
  wakeLockApi: boolean;
}

export interface WebXrPoseSnapshot {
  position: readonly [number, number, number];
  orientation: readonly [number, number, number, number];
  views: number;
}

export interface WebXrRuntimeState {
  phase: XrRuntimePhase;
  referenceSpace: 'local-floor' | 'local' | null;
  domOverlay: boolean;
  environmentBlendMode: string | null;
  frameCount: number;
  tracking: boolean;
  pose: WebXrPoseSnapshot | null;
  scanPaused: boolean;
  wakeLockActive: boolean;
  wakeLockError: string | null;
  depthAvailable: boolean;
  depthUsage: string | null;
  depthDataFormat: string | null;
  depthType: string | null;
  depthWidth: number | null;
  depthHeight: number | null;
  depthFrameCount: number;
  sampledDepthFrames: number;
  pointCount: number;
  retainedPointCount: number;
  pointCloudVersion: number;
  keyframeCount: number;
  polishFrameCount: number;
  motionRejectedFrames: number;
  motionState: FusionMotionState | 'paused' | null;
  motionQuality: number;
  linearSpeedMetersPerSecond: number;
  angularSpeedRadiansPerSecond: number;
  linearAccelerationMetersPerSecond2: number;
  coverageGuideActive: boolean;
  coverageGuideRenderPoints: number;
  coverageVisibleCells: number;
  coverageReadyCells: number;
  coverageTotalCells: number;
  coverageTargetIndependentViews: number;
  coverageAverageConfidence: number;
  coverageGuideError: string | null;
  lightingEstimateAvailable: boolean;
  primaryLightIntensity: number | null;
  depthError: string | null;
  error: string | null;
}

interface XrPointLike {
  x: number;
  y: number;
  z: number;
  w?: number;
}

interface XrRigidTransformLike {
  position: XrPointLike;
  orientation: Required<XrPointLike>;
  matrix: ArrayLike<number>;
}

interface XrViewLike extends DepthSampleView, CoverageRenderView {
  projectionMatrix: ArrayLike<number>;
  transform: XrRigidTransformLike;
}

interface XrReferenceSpaceLike {}
interface XrLightProbeLike {}

interface XrLightEstimateLike {
  primaryLightIntensity: XrPointLike;
}

interface XrViewerPoseLike {
  transform: XrRigidTransformLike;
  views: readonly XrViewLike[];
}

interface XrFrameLike {
  session: XrSessionLike;
  getViewerPose(referenceSpace: XrReferenceSpaceLike): XrViewerPoseLike | null;
  getDepthInformation?(view: XrViewLike): CpuDepthInformationLike | null;
  getLightEstimate?(probe: XrLightProbeLike): XrLightEstimateLike | null;
}

type XrFrameCallback = (time: number, frame: XrFrameLike) => void;

interface XrWebGlLayerLike {
  framebuffer: WebGLFramebuffer | null;
  framebufferWidth: number;
  framebufferHeight: number;
  getViewport(view: CoverageRenderView): CoverageRenderViewport;
}

interface XrSessionLike extends EventTarget {
  requestReferenceSpace(type: 'local-floor' | 'local'): Promise<XrReferenceSpaceLike>;
  requestAnimationFrame(callback: XrFrameCallback): number;
  updateRenderState(state: { baseLayer: XrWebGlLayerLike }): void | Promise<void>;
  end(): Promise<void>;
  requestLightProbe?(): Promise<XrLightProbeLike>;
  domOverlayState?: { type: string } | null;
  environmentBlendMode?: string;
  visibilityState?: 'visible' | 'visible-blurred' | 'hidden';
  depthUsage?: string;
  depthDataFormat?: string;
  depthType?: string | null;
  depthActive?: boolean | null;
}

interface XrSessionInitLike {
  optionalFeatures?: string[];
  domOverlay?: { root: Element };
  depthSensing?: {
    usagePreference: string[];
    dataFormatPreference: string[];
    /** Compatibility alias used by older Chromium Depth API revisions. */
    formatPreference?: string[];
    depthTypeRequest?: string[];
    matchDepthView?: boolean;
  };
}

interface XrSystemLike {
  isSessionSupported(mode: 'immersive-ar'): Promise<boolean>;
  requestSession(mode: 'immersive-ar', options?: XrSessionInitLike): Promise<XrSessionLike>;
}

type XrCompatibleGl = (WebGLRenderingContext | WebGL2RenderingContext) & {
  makeXRCompatible?: () => Promise<void>;
};

type XrWebGlLayerConstructor = new (
  session: XrSessionLike,
  context: XrCompatibleGl,
) => XrWebGlLayerLike;

type NavigatorWithXr = Navigator & { xr?: XrSystemLike };
type WindowWithXrLayer = Window & { XRWebGLLayer?: XrWebGlLayerConstructor };

interface WakeLockSentinelLike extends EventTarget {
  released: boolean;
  release(): Promise<void>;
}

interface WakeLockLike {
  request(type: 'screen'): Promise<WakeLockSentinelLike>;
}

type NavigatorWithWakeLock = Navigator & { wakeLock?: WakeLockLike };
type RuntimeListener = (state: WebXrRuntimeState) => void;

interface DepthConfiguration {
  enabled: boolean;
  usage: string | null;
  dataFormat: string | null;
  type: string | null;
}

const DEPTH_SAMPLE_INTERVAL_MS = 140;
const COVERAGE_GPU_UPLOAD_INTERVAL_MS = 550;
const LIGHT_ESTIMATE_INTERVAL_MS = 500;

const initialRuntimeState: WebXrRuntimeState = {
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

function createWebGlContext(): XrCompatibleGl | null {
  const canvas = document.createElement('canvas');
  const attributes: WebGLContextAttributes = {
    alpha: true,
    antialias: false,
    depth: true,
    stencil: false,
    premultipliedAlpha: true,
    preserveDrawingBuffer: false,
  };
  const webGl2 = canvas.getContext('webgl2', attributes);
  if (webGl2 !== null) return webGl2 as XrCompatibleGl;
  const webGl = canvas.getContext('webgl', attributes);
  return webGl === null ? null : (webGl as XrCompatibleGl);
}

function releaseWebGlContext(context: XrCompatibleGl | null): void {
  context?.getExtension('WEBGL_lose_context')?.loseContext();
}

function hasWebGl(): boolean {
  const context = createWebGlContext();
  if (context === null) return false;
  releaseWebGlContext(context);
  return true;
}

function errorText(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

function readDepthConfiguration(session: XrSessionLike): DepthConfiguration {
  try {
    const usage = session.depthUsage ?? null;
    const dataFormat = session.depthDataFormat ?? null;
    const type = session.depthType ?? null;
    return {
      enabled: usage === 'cpu-optimized' && session.depthActive !== false,
      usage,
      dataFormat,
      type,
    };
  } catch {
    return { enabled: false, usage: null, dataFormat: null, type: null };
  }
}

function depthGeometry(
  depth: CpuDepthInformationLike,
  fallbackView: XrViewLike,
): DepthSampleView {
  if (
    depth.projectionMatrix !== undefined && depth.projectionMatrix.length >= 16 &&
    depth.transform !== undefined && depth.transform.matrix.length >= 16
  ) {
    return { projectionMatrix: depth.projectionMatrix, transform: depth.transform };
  }
  return fallbackView;
}

function projectCameraPosition(pose: XrViewerPoseLike): readonly [number, number, number] {
  const position = pose.transform.position;
  return [position.x, -position.z, position.y];
}

export async function probeWebXrCapabilities(): Promise<WebXrCapabilities> {
  const xr = (navigator as NavigatorWithXr).xr;
  const xrApi = xr !== undefined;
  let immersiveAr = false;
  if (xrApi && window.isSecureContext) {
    try {
      immersiveAr = await xr.isSessionSupported('immersive-ar');
    } catch {
      immersiveAr = false;
    }
  }
  return {
    secureContext: window.isSecureContext === true,
    cameraApi: Boolean(navigator.mediaDevices?.getUserMedia),
    webGl: hasWebGl(),
    xrApi,
    immersiveAr,
    wakeLockApi: (navigator as NavigatorWithWakeLock).wakeLock !== undefined,
  };
}

export class WebXrRuntime {
  private state: WebXrRuntimeState = initialRuntimeState;
  private listeners = new Set<RuntimeListener>();
  private session: XrSessionLike | null = null;
  private referenceSpace: XrReferenceSpaceLike | null = null;
  private gl: XrCompatibleGl | null = null;
  private layer: XrWebGlLayerLike | null = null;
  private coverageRenderer: LiveCoveragePointRenderer | null = null;
  private lightProbe: XrLightProbeLike | null = null;
  private wakeLock: WakeLockSentinelLike | null = null;
  private explicitStopRequested = false;
  private scanPaused = false;
  private frameCount = 0;
  private lastUiUpdateMs = 0;
  private lastDepthSampleMs = Number.NEGATIVE_INFINITY;
  private lastCoverageGpuUploadMs = Number.NEGATIVE_INFINITY;
  private lastLightEstimateMs = Number.NEGATIVE_INFINITY;
  private depthFrameCount = 0;
  private sampledDepthFrames = 0;
  private pointCloudVersion = 0;
  private depthWidth: number | null = null;
  private depthHeight: number | null = null;
  private depthFailureReported = false;
  private depthEnabled = false;
  private motionRejectedFrames = 0;
  private motionState: FusionMotionState | 'paused' | null = null;
  private motionQuality = 0;
  private linearSpeedMetersPerSecond = 0;
  private angularSpeedRadiansPerSecond = 0;
  private linearAccelerationMetersPerSecond2 = 0;
  private coverageGuideActive = false;
  private coverageGuideRenderPoints = 0;
  private coverageVisibleCells = 0;
  private coverageReadyCells = 0;
  private coverageTotalCells = 0;
  private coverageTargetIndependentViews = 3;
  private coverageAverageConfidence = 0;
  private coverageGuideError: string | null = null;
  private lightingEstimateAvailable = false;
  private primaryLightIntensity: number | null = null;
  private readonly pointCloud = new DepthPointCloudAccumulator();
  private readonly keyframeSelector = new WebXrFusionKeyframeSelector();

  constructor() {
    document.addEventListener('visibilitychange', this.handleDocumentVisibilityChange);
  }

  subscribe(listener: RuntimeListener): () => void {
    this.listeners.add(listener);
    listener(this.state);
    return () => this.listeners.delete(listener);
  }

  getState(): WebXrRuntimeState { return this.state; }

  getPointCloudSnapshot(): ScanPointCloudSnapshot {
    return this.pointCloud.snapshot(this.depthFrameCount, this.sampledDepthFrames);
  }

  async start(): Promise<void> {
    if (this.session !== null || this.state.phase === 'starting') return;

    const xr = (navigator as NavigatorWithXr).xr;
    const Layer = (window as WindowWithXrLayer).XRWebGLLayer;
    if (!window.isSecureContext) {
      this.publish({ phase: 'error', error: 'Trusted HTTPS is required for WebXR.' });
      return;
    }
    if (xr === undefined) {
      this.publish({ phase: 'error', error: 'WebXR is not available in this browser.' });
      return;
    }
    if (Layer === undefined) {
      this.publish({ phase: 'error', error: 'XRWebGLLayer is unavailable.' });
      return;
    }

    this.resetScanState();
    this.publish({
      ...initialRuntimeState,
      phase: 'starting',
      wakeLockActive: false,
      wakeLockError: null,
    });

    let session: XrSessionLike | null = null;
    try {
      const depthFormats = ['luminance-alpha', 'float32'];
      session = await xr.requestSession('immersive-ar', {
        optionalFeatures: [
          'local-floor',
          'dom-overlay',
          'depth-sensing',
          'light-estimation',
        ],
        domOverlay: { root: document.body },
        depthSensing: {
          usagePreference: ['cpu-optimized'],
          dataFormatPreference: depthFormats,
          formatPreference: depthFormats,
          depthTypeRequest: ['raw', 'smooth'],
          matchDepthView: true,
        },
      });

      this.session = session;
      session.addEventListener('end', this.handleSessionEnd);
      session.addEventListener('visibilitychange', this.handleSessionVisibilityChange);

      const gl = createWebGlContext();
      if (gl === null) throw new Error('Unable to create WebGL for WebXR.');
      await gl.makeXRCompatible?.();
      const layer = new Layer(session, gl);
      await session.updateRenderState({ baseLayer: layer });

      let referenceSpaceType: 'local-floor' | 'local' = 'local-floor';
      let referenceSpace: XrReferenceSpaceLike;
      try {
        referenceSpace = await session.requestReferenceSpace('local-floor');
      } catch {
        referenceSpaceType = 'local';
        referenceSpace = await session.requestReferenceSpace('local');
      }

      this.referenceSpace = referenceSpace;
      this.gl = gl;
      this.layer = layer;
      try {
        this.coverageRenderer = new LiveCoveragePointRenderer(gl);
      } catch (error) {
        this.coverageRenderer = null;
        this.coverageGuideError = `Coverage renderer unavailable: ${errorText(error)}`;
      }

      if (typeof session.requestLightProbe === 'function') {
        try {
          this.lightProbe = await session.requestLightProbe();
        } catch {
          this.lightProbe = null;
        }
      }

      document.documentElement.classList.add('xr-session-active');
      const depthConfiguration = readDepthConfiguration(session);
      this.depthEnabled = depthConfiguration.enabled;
      void this.acquireWakeLock();

      this.publish({
        phase: 'running',
        referenceSpace: referenceSpaceType,
        domOverlay: session.domOverlayState != null,
        environmentBlendMode: session.environmentBlendMode ?? null,
        depthAvailable: depthConfiguration.enabled,
        depthUsage: depthConfiguration.usage,
        depthDataFormat: depthConfiguration.dataFormat,
        depthType: depthConfiguration.type,
        coverageGuideError: this.coverageGuideError,
        wakeLockActive: this.wakeLock !== null && !this.wakeLock.released,
        depthError: depthConfiguration.enabled
          ? null
          : 'WebXR tracking is running, but CPU depth was not enabled.',
        error: null,
      });

      session.requestAnimationFrame(this.handleFrame);
    } catch (error) {
      if (session !== null) {
        session.removeEventListener('end', this.handleSessionEnd);
        session.removeEventListener('visibilitychange', this.handleSessionVisibilityChange);
        try { await session.end(); } catch { /* best effort */ }
      }
      this.cleanupRuntime();
      this.publish({
        phase: 'error',
        referenceSpace: null,
        domOverlay: false,
        environmentBlendMode: null,
        tracking: false,
        pose: null,
        scanPaused: false,
        depthAvailable: false,
        error: errorText(error),
      });
    }
  }

  async stop(): Promise<void> {
    const session = this.session;
    if (session === null) return;
    this.explicitStopRequested = true;
    this.publish({ phase: 'stopping', error: null });
    try {
      await session.end();
    } catch (error) {
      this.explicitStopRequested = false;
      this.publish({ phase: 'error', error: errorText(error) });
    }
  }

  async dispose(): Promise<void> {
    document.removeEventListener('visibilitychange', this.handleDocumentVisibilityChange);
    const session = this.session;
    if (session !== null) {
      session.removeEventListener('end', this.handleSessionEnd);
      session.removeEventListener('visibilitychange', this.handleSessionVisibilityChange);
      try { await session.end(); } catch { /* already ended */ }
    }
    this.cleanupRuntime();
    this.listeners.clear();
  }

  private publish(patch: Partial<WebXrRuntimeState>): void {
    this.state = { ...this.state, ...patch };
    for (const listener of this.listeners) listener(this.state);
  }

  private resetScanState(): void {
    this.pointCloud.reset();
    this.keyframeSelector.reset();
    this.explicitStopRequested = false;
    this.scanPaused = false;
    this.frameCount = 0;
    this.lastUiUpdateMs = 0;
    this.lastDepthSampleMs = Number.NEGATIVE_INFINITY;
    this.lastCoverageGpuUploadMs = Number.NEGATIVE_INFINITY;
    this.lastLightEstimateMs = Number.NEGATIVE_INFINITY;
    this.depthFrameCount = 0;
    this.sampledDepthFrames = 0;
    this.pointCloudVersion = 0;
    this.depthWidth = null;
    this.depthHeight = null;
    this.depthFailureReported = false;
    this.depthEnabled = false;
    this.motionRejectedFrames = 0;
    this.motionState = null;
    this.motionQuality = 0;
    this.linearSpeedMetersPerSecond = 0;
    this.angularSpeedRadiansPerSecond = 0;
    this.linearAccelerationMetersPerSecond2 = 0;
    this.coverageGuideActive = false;
    this.coverageGuideRenderPoints = 0;
    this.coverageVisibleCells = 0;
    this.coverageReadyCells = 0;
    this.coverageTotalCells = 0;
    this.coverageTargetIndependentViews = 3;
    this.coverageAverageConfidence = 0;
    this.coverageGuideError = null;
    this.lightProbe = null;
    this.lightingEstimateAvailable = false;
    this.primaryLightIntensity = null;
  }

  private cleanupRuntime(): void {
    document.documentElement.classList.remove('xr-session-active');
    this.coverageRenderer?.dispose();
    this.coverageRenderer = null;
    this.lightProbe = null;
    void this.releaseWakeLock();
    releaseWebGlContext(this.gl);
    this.session = null;
    this.referenceSpace = null;
    this.gl = null;
    this.layer = null;
    this.scanPaused = false;
    this.depthEnabled = false;
    this.coverageGuideActive = false;
    this.coverageGuideRenderPoints = 0;
  }

  private async acquireWakeLock(): Promise<void> {
    if (
      this.session === null ||
      document.visibilityState !== 'visible' ||
      (this.wakeLock !== null && !this.wakeLock.released)
    ) return;

    const wakeLockApi = (navigator as NavigatorWithWakeLock).wakeLock;
    if (wakeLockApi === undefined) {
      this.publish({ wakeLockActive: false, wakeLockError: 'Screen Wake Lock API unavailable.' });
      return;
    }

    try {
      const sentinel = await wakeLockApi.request('screen');
      this.wakeLock = sentinel;
      sentinel.addEventListener('release', this.handleWakeLockRelease);
      this.publish({ wakeLockActive: true, wakeLockError: null });
    } catch (error) {
      this.wakeLock = null;
      this.publish({ wakeLockActive: false, wakeLockError: errorText(error) });
    }
  }

  private async releaseWakeLock(): Promise<void> {
    const sentinel = this.wakeLock;
    this.wakeLock = null;
    if (sentinel === null) return;
    sentinel.removeEventListener('release', this.handleWakeLockRelease);
    if (!sentinel.released) {
      try { await sentinel.release(); } catch { /* platform already released it */ }
    }
  }

  private readonly handleWakeLockRelease = (): void => {
    this.wakeLock = null;
    this.publish({ wakeLockActive: false });
  };

  private readonly handleDocumentVisibilityChange = (): void => {
    const hidden = document.visibilityState !== 'visible';
    if (this.session !== null) {
      this.scanPaused = hidden || this.session.visibilityState === 'hidden';
      this.publish({
        scanPaused: this.scanPaused,
        motionState: this.scanPaused ? 'paused' : this.motionState,
      });
      if (!hidden) void this.acquireWakeLock();
    }
  };

  private readonly handleSessionVisibilityChange = (): void => {
    const session = this.session;
    if (session === null) return;
    this.scanPaused =
      document.visibilityState !== 'visible' || session.visibilityState === 'hidden';
    this.publish({
      scanPaused: this.scanPaused,
      motionState: this.scanPaused ? 'paused' : this.motionState,
    });
    if (!this.scanPaused) void this.acquireWakeLock();
  };

  private readonly handleSessionEnd = (): void => {
    const normalFinish = this.explicitStopRequested;
    const snapshot = this.pointCloud.snapshot(this.depthFrameCount, this.sampledDepthFrames);
    const finalState = {
      referenceSpace: null,
      domOverlay: false,
      environmentBlendMode: null,
      tracking: false,
      pose: null,
      scanPaused: false,
      wakeLockActive: false,
      depthFrameCount: this.depthFrameCount,
      sampledDepthFrames: this.sampledDepthFrames,
      pointCount: snapshot.pointCount,
      retainedPointCount: snapshot.rawPointCount,
      keyframeCount: snapshot.integrationKeyframes,
      polishFrameCount: snapshot.polishFrames,
      pointCloudVersion: this.pointCloudVersion,
      coverageGuideActive: false,
      coverageGuideRenderPoints: 0,
      coverageVisibleCells: this.coverageVisibleCells,
      coverageReadyCells: this.coverageReadyCells,
      coverageTotalCells: this.coverageTotalCells,
      coverageTargetIndependentViews: this.coverageTargetIndependentViews,
      coverageAverageConfidence: this.coverageAverageConfidence,
      lightingEstimateAvailable: this.lightingEstimateAvailable,
      primaryLightIntensity: this.primaryLightIntensity,
      error: null,
    };

    const session = this.session;
    session?.removeEventListener('end', this.handleSessionEnd);
    session?.removeEventListener('visibilitychange', this.handleSessionVisibilityChange);
    this.cleanupRuntime();
    this.explicitStopRequested = false;

    this.publish({
      ...finalState,
      phase: normalFinish ? 'ended' : 'interrupted',
      motionState: normalFinish ? this.motionState : 'paused',
    });
  };

  private refreshCoverageGuide(time: number): void {
    if (time - this.lastCoverageGpuUploadMs < COVERAGE_GPU_UPLOAD_INTERVAL_MS) return;
    this.lastCoverageGpuUploadMs = time;
    const coverage: LiveCoverageSnapshot = this.pointCloud.liveCoverageSnapshot();
    this.coverageGuideActive = coverage.active && this.coverageRenderer !== null;
    this.coverageVisibleCells = coverage.visibleCoverageCells;
    this.coverageReadyCells = coverage.readyCoverageCells;
    this.coverageTotalCells = coverage.totalCoverageCells;
    this.coverageTargetIndependentViews = coverage.targetFrameHits;
    this.coverageAverageConfidence = coverage.averageCoverageConfidence;

    if (this.coverageGuideActive && this.coverageRenderer !== null) {
      this.coverageRenderer.update(coverage);
      this.coverageGuideRenderPoints = coverage.pointCount;
    } else {
      this.coverageRenderer?.clear();
      this.coverageGuideRenderPoints = 0;
    }
  }

  private updateLightEstimate(time: number, frame: XrFrameLike): void {
    if (
      this.lightProbe === null ||
      typeof frame.getLightEstimate !== 'function' ||
      time - this.lastLightEstimateMs < LIGHT_ESTIMATE_INTERVAL_MS
    ) return;

    this.lastLightEstimateMs = time;
    try {
      const estimate = frame.getLightEstimate(this.lightProbe);
      if (estimate === null) return;
      const intensity = estimate.primaryLightIntensity;
      const magnitude = Math.sqrt(
        (intensity.x * intensity.x + intensity.y * intensity.y + intensity.z * intensity.z) / 3,
      );
      if (Number.isFinite(magnitude)) {
        this.lightingEstimateAvailable = true;
        this.primaryLightIntensity = magnitude;
      }
    } catch {
      // Lighting is advisory only; depth fusion must not fail if a runtime drops it.
    }
  }

  private captureDepth(time: number, frame: XrFrameLike, pose: XrViewerPoseLike): void {
    if (this.scanPaused || !this.depthEnabled || pose.views.length === 0) return;
    if (typeof frame.getDepthInformation !== 'function') {
      if (!this.depthFailureReported) {
        this.depthFailureReported = true;
        this.publish({ depthError: 'XRFrame.getDepthInformation() is unavailable.' });
      }
      return;
    }

    const view = pose.views[0];
    try {
      const depth = frame.getDepthInformation(view);
      if (depth === null) return;
      this.depthFrameCount += 1;
      this.depthWidth = depth.width;
      this.depthHeight = depth.height;
      if (time - this.lastDepthSampleMs < DEPTH_SAMPLE_INTERVAL_MS) return;
      this.lastDepthSampleMs = time;

      const decision = this.keyframeSelector.evaluate(time, pose.transform);
      this.motionState = decision.state;
      this.motionQuality = decision.motionQuality;
      this.linearSpeedMetersPerSecond = decision.linearSpeedMetersPerSecond;
      this.angularSpeedRadiansPerSecond = decision.angularSpeedRadiansPerSecond;
      this.linearAccelerationMetersPerSecond2 = decision.linearAccelerationMetersPerSecond2;

      if (decision.kind === 'skip') {
        if (decision.state === 'too-fast') this.motionRejectedFrames += 1;
        return;
      }

      this.sampledDepthFrames += 1;
      const context: DepthFusionFrameContext = {
        keyframeId: decision.keyframeId,
        evidenceKeyframe: decision.kind === 'keyframe',
        frameWeight: decision.frameWeight,
        cameraProjectPosition: projectCameraPosition(pose),
      };
      const result = this.pointCloud.sampleDepthFrame(
        depth,
        depthGeometry(depth, view),
        context,
      );
      if (result.changedPoints > 0) this.pointCloudVersion += 1;
      this.refreshCoverageGuide(time);
    } catch (error) {
      if (!this.depthFailureReported) {
        this.depthFailureReported = true;
        this.publish({ depthError: `Depth access failed: ${errorText(error)}` });
      }
    }
  }

  private readonly handleFrame: XrFrameCallback = (time, frame): void => {
    const session = this.session;
    const referenceSpace = this.referenceSpace;
    const gl = this.gl;
    const layer = this.layer;
    if (
      session === null || referenceSpace === null || gl === null || layer === null ||
      frame.session !== session
    ) return;

    session.requestAnimationFrame(this.handleFrame);
    gl.bindFramebuffer(gl.FRAMEBUFFER, layer.framebuffer);
    gl.viewport(0, 0, layer.framebufferWidth, layer.framebufferHeight);
    gl.clearColor(0, 0, 0, 0);
    gl.clearDepth(1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

    const pose = frame.getViewerPose(referenceSpace);
    this.frameCount += 1;
    this.updateLightEstimate(time, frame);

    if (pose !== null && !this.scanPaused) {
      this.captureDepth(time, frame, pose);
      this.coverageRenderer?.render(layer, pose.views);
    }

    if (time - this.lastUiUpdateMs < 100 && this.frameCount > 1) return;
    this.lastUiUpdateMs = time;

    const commonPatch = {
      frameCount: this.frameCount,
      scanPaused: this.scanPaused,
      wakeLockActive: this.wakeLock !== null && !this.wakeLock.released,
      depthWidth: this.depthWidth,
      depthHeight: this.depthHeight,
      depthFrameCount: this.depthFrameCount,
      sampledDepthFrames: this.sampledDepthFrames,
      pointCount: this.pointCloud.promotedCount,
      retainedPointCount: this.pointCloud.pointCount,
      pointCloudVersion: this.pointCloudVersion,
      keyframeCount: this.pointCloud.keyframeCount,
      polishFrameCount: this.pointCloud.polishFrameCount,
      motionRejectedFrames: this.motionRejectedFrames,
      motionState: this.scanPaused ? 'paused' as const : this.motionState,
      motionQuality: this.motionQuality,
      linearSpeedMetersPerSecond: this.linearSpeedMetersPerSecond,
      angularSpeedRadiansPerSecond: this.angularSpeedRadiansPerSecond,
      linearAccelerationMetersPerSecond2: this.linearAccelerationMetersPerSecond2,
      coverageGuideActive: this.coverageGuideActive,
      coverageGuideRenderPoints: this.coverageGuideRenderPoints,
      coverageVisibleCells: this.coverageVisibleCells,
      coverageReadyCells: this.coverageReadyCells,
      coverageTotalCells: this.coverageTotalCells,
      coverageTargetIndependentViews: this.coverageTargetIndependentViews,
      coverageAverageConfidence: this.coverageAverageConfidence,
      coverageGuideError: this.coverageGuideError,
      lightingEstimateAvailable: this.lightingEstimateAvailable,
      primaryLightIntensity: this.primaryLightIntensity,
    };

    if (pose === null) {
      this.publish({ ...commonPatch, tracking: false, pose: null });
      return;
    }

    const { position, orientation } = pose.transform;
    this.publish({
      ...commonPatch,
      tracking: true,
      pose: {
        position: [position.x, position.y, position.z],
        orientation: [orientation.x, orientation.y, orientation.z, orientation.w],
        views: pose.views.length,
      },
    });
  };
}

export type { ScanPointCloudSnapshot } from './depthPointCloud';
