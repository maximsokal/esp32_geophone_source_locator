import { clientDiagnostics } from '../data/clientDiagnostics';
import { countCoverageQualityBands } from './coverageQuality';
import {
  DepthPointCloudAccumulator,
  type CpuDepthInformationLike,
  type DepthFusionFrameContext,
  type DepthSampleView,
  type LiveCoverageSnapshot,
  type ScanPointCloudSnapshot as DepthScanPointCloudSnapshot,
} from './depthPointCloud';
import {
  LiveCoveragePointRenderer,
  type CoverageRenderSnapshot,
  type CoverageRenderView,
  type CoverageRenderViewport,
} from './liveCoverageRenderer';
import {
  ManualControlPointStore,
  projectDepthSampleToProject,
  type ManualControlPoint,
  type ProjectPoint,
} from './manualControlPoints';
import { OperatorEdgeStore, type OperatorEdge } from './operatorEdges';
import {
  projectProjectPointToNormalized as projectProjectPointToScreen,
  type NormalizedProjection,
} from './projectionMath';
import {
  XrPerformanceProfiler,
  emptyXrPerformanceSnapshot,
  type XrPerformanceSnapshot,
} from './xrPerformanceProfiler';
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

export interface ScanPointCloudSnapshot extends DepthScanPointCloudSnapshot {
  manualControlPoints: readonly ManualControlPoint[];
  operatorEdges: readonly OperatorEdge[];
}

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
  coverageWeakPoints: number;
  coverageImprovingPoints: number;
  coverageStablePoints: number;
  coverageGuideError: string | null;
  manualPointCount: number;
  manualEdgeCount: number;
  manualPointError: string | null;
  performance: XrPerformanceSnapshot;
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

interface PendingManualPointRequest {
  u: number;
  v: number;
  pointId: number | null;
}

const DEPTH_SAMPLE_INTERVAL_MS = 140;
const COVERAGE_GPU_UPLOAD_INTERVAL_MS = 550;
const LIGHT_ESTIMATE_INTERVAL_MS = 500;
const CLIENT_TELEMETRY_INTERVAL_MS = 5000;
const MIN_MANUAL_DEPTH_METERS = 0.18;

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
  coverageWeakPoints: 0,
  coverageImprovingPoints: 0,
  coverageStablePoints: 0,
  coverageGuideError: null,
  manualPointCount: 0,
  manualEdgeCount: 0,
  manualPointError: null,
  performance: emptyXrPerformanceSnapshot,
  lightingEstimateAvailable: false,
  primaryLightIntensity: null,
  depthError: null,
  error: null,
};

function clamp01(value: number): number {
  return Math.max(0, Math.min(1, value));
}

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

export function composeCoverageRenderSnapshot(
  coverage: LiveCoverageSnapshot,
  manualReferencePositions: Float32Array,
): CoverageRenderSnapshot {
  const coverageCount = Math.min(
    coverage.pointCount,
    Math.floor(coverage.positions.length / 3),
    coverage.confidence.length,
  );
  const manualCount = Math.floor(manualReferencePositions.length / 3);
  const pointCount = coverageCount + manualCount;
  const positions = new Float32Array(pointCount * 3);
  const confidence = new Float32Array(pointCount);

  if (coverageCount > 0) {
    positions.set(coverage.positions.subarray(0, coverageCount * 3), 0);
    confidence.set(coverage.confidence.subarray(0, coverageCount), 0);
  }
  if (manualCount > 0) {
    positions.set(manualReferencePositions.subarray(0, manualCount * 3), coverageCount * 3);
    confidence.fill(2, coverageCount, pointCount);
  }
  return { positions, confidence, pointCount };
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
  private lastClientTelemetryMs = Number.NEGATIVE_INFINITY;
  private depthFrameCount = 0;
  private sampledDepthFrames = 0;
  private pointCloudVersion = 0;
  private depthWidth: number | null = null;
  private depthHeight: number | null = null;
  private depthFailureReported = false;
  private depthReadyLogged = false;
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
  private coverageWeakPoints = 0;
  private coverageImprovingPoints = 0;
  private coverageStablePoints = 0;
  private coverageGuideError: string | null = null;
  private pendingManualPointRequest: PendingManualPointRequest | null = null;
  private lightingEstimateAvailable = false;
  private primaryLightIntensity: number | null = null;
  private readonly pointCloud = new DepthPointCloudAccumulator();
  private readonly keyframeSelector = new WebXrFusionKeyframeSelector();
  private readonly manualPoints = new ManualControlPointStore();
  private readonly operatorEdges = new OperatorEdgeStore();
  private readonly performanceProfiler = new XrPerformanceProfiler();
  private readonly latestProjectionMatrix = new Float32Array(16);
  private readonly latestWorldFromView = new Float32Array(16);
  private hasLatestProjection = false;

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
    return {
      ...this.pointCloud.snapshot(this.depthFrameCount, this.sampledDepthFrames),
      manualControlPoints: this.manualPoints.snapshot(),
      operatorEdges: this.operatorEdges.snapshot(),
    };
  }

  getLastManualPointId(): number | null {
    return this.manualPoints.lastId;
  }

  getStructuralGuideSnapshot() {
    return this.pointCloud.structuralGuideSnapshot();
  }

  getManualControlPoints(): readonly ManualControlPoint[] {
    return this.manualPoints.snapshot();
  }

  getOperatorEdges(): readonly OperatorEdge[] {
    return this.operatorEdges.snapshot();
  }

  projectProjectPointToNormalized(point: ProjectPoint): NormalizedProjection | null {
    if (!this.hasLatestProjection) return null;
    return projectProjectPointToScreen(point, this.latestProjectionMatrix, this.latestWorldFromView);
  }

  addSuggestedManualPoint(position: ProjectPoint): { point: ManualControlPoint; created: boolean; snapped: boolean } | null {
    if (this.session === null || this.state.phase !== 'running') return null;
    const stored = this.manualPoints.addOrSnap(position);
    this.lastCoverageGpuUploadMs = Number.NEGATIVE_INFINITY;
    this.publish({ manualPointCount: this.manualPoints.size, manualPointError: null });
    return stored;
  }

  addOperatorEdge(start: ProjectPoint, end: ProjectPoint, confidence = 1): OperatorEdge | null {
    if (this.session === null || this.state.phase !== 'running') return null;
    const stored = this.operatorEdges.add(start, end, confidence);
    if (stored !== null) this.publish({ manualEdgeCount: this.operatorEdges.size });
    return stored;
  }

  queueManualPointAtNormalized(u: number, v: number, pointId: number | null = null): void {
    if (this.session === null || this.state.phase !== 'running') {
      this.publish({ manualPointError: 'Manual point can only be placed during an active AR scan.' });
      return;
    }
    if (pointId !== null && this.manualPoints.lastId === null) {
      this.publish({ manualPointError: 'There is no manual point to move yet.' });
      return;
    }
    this.pendingManualPointRequest = {
      u: clamp01(u),
      v: clamp01(v),
      pointId,
    };
    this.publish({ manualPointError: null });
  }

  undoLastManualPoint(): void {
    const removed = this.manualPoints.removeLast();
    if (removed === null) return;
    this.lastCoverageGpuUploadMs = Number.NEGATIVE_INFINITY;
    this.publish({ manualPointCount: this.manualPoints.size, manualPointError: null });
    clientDiagnostics.info('XR_MANUAL_POINT_UNDO', { id: removed.id, remaining: this.manualPoints.size });
    void clientDiagnostics.flush();
  }

  async start(): Promise<void> {
    if (this.session !== null || this.state.phase === 'starting') return;

    clientDiagnostics.info('XR_START_REQUEST');
    void clientDiagnostics.flush();

    const xr = (navigator as NavigatorWithXr).xr;
    const Layer = (window as WindowWithXrLayer).XRWebGLLayer;
    if (!window.isSecureContext) {
      const message = 'Trusted HTTPS is required for WebXR.';
      clientDiagnostics.error('XR_START_REJECTED', message);
      this.publish({ phase: 'error', error: message });
      return;
    }
    if (xr === undefined) {
      const message = 'WebXR is not available in this browser.';
      clientDiagnostics.error('XR_START_REJECTED', message);
      this.publish({ phase: 'error', error: message });
      return;
    }
    if (Layer === undefined) {
      const message = 'XRWebGLLayer is unavailable.';
      clientDiagnostics.error('XR_START_REJECTED', message);
      this.publish({ phase: 'error', error: message });
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
        clientDiagnostics.warn('XR_COVERAGE_RENDERER_UNAVAILABLE', error);
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

      clientDiagnostics.info('XR_SESSION_RUNNING', {
        referenceSpace: referenceSpaceType,
        domOverlay: session.domOverlayState != null,
        environmentBlendMode: session.environmentBlendMode ?? null,
        depthEnabled: depthConfiguration.enabled,
        depthUsage: depthConfiguration.usage,
        depthFormat: depthConfiguration.dataFormat,
        depthType: depthConfiguration.type,
      });
      void clientDiagnostics.flush();
      session.requestAnimationFrame(this.handleFrame);
    } catch (error) {
      if (session !== null) {
        session.removeEventListener('end', this.handleSessionEnd);
        session.removeEventListener('visibilitychange', this.handleSessionVisibilityChange);
        try { await session.end(); } catch { /* best effort */ }
      }
      this.cleanupRuntime();
      const message = errorText(error);
      clientDiagnostics.error('XR_START_FAILED', message);
      void clientDiagnostics.flush();
      this.publish({
        phase: 'error',
        referenceSpace: null,
        domOverlay: false,
        environmentBlendMode: null,
        tracking: false,
        pose: null,
        scanPaused: false,
        depthAvailable: false,
        error: message,
      });
    }
  }

  async stop(): Promise<void> {
    const session = this.session;
    if (session === null) return;
    this.explicitStopRequested = true;
    clientDiagnostics.info('XR_FINISH_REQUEST', {
      stablePoints: this.pointCloud.promotedCount,
      retainedPoints: this.pointCloud.pointCount,
      keyframes: this.pointCloud.keyframeCount,
      manualPoints: this.manualPoints.size,
      operatorEdges: this.operatorEdges.size,
    });
    void clientDiagnostics.flush();
    this.publish({ phase: 'stopping', error: null });
    try {
      await session.end();
    } catch (error) {
      this.explicitStopRequested = false;
      const message = errorText(error);
      clientDiagnostics.error('XR_FINISH_FAILED', message);
      this.publish({ phase: 'error', error: message });
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
    this.manualPoints.reset();
    this.operatorEdges.reset();
    this.performanceProfiler.reset();
    this.hasLatestProjection = false;
    this.pendingManualPointRequest = null;
    this.explicitStopRequested = false;
    this.scanPaused = false;
    this.frameCount = 0;
    this.lastUiUpdateMs = 0;
    this.lastDepthSampleMs = Number.NEGATIVE_INFINITY;
    this.lastCoverageGpuUploadMs = Number.NEGATIVE_INFINITY;
    this.lastLightEstimateMs = Number.NEGATIVE_INFINITY;
    this.lastClientTelemetryMs = Number.NEGATIVE_INFINITY;
    this.depthFrameCount = 0;
    this.sampledDepthFrames = 0;
    this.pointCloudVersion = 0;
    this.depthWidth = null;
    this.depthHeight = null;
    this.depthFailureReported = false;
    this.depthReadyLogged = false;
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
    this.coverageWeakPoints = 0;
    this.coverageImprovingPoints = 0;
    this.coverageStablePoints = 0;
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
    this.pendingManualPointRequest = null;
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
    const snapshot = this.getPointCloudSnapshot();
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
      coverageWeakPoints: this.coverageWeakPoints,
      coverageImprovingPoints: this.coverageImprovingPoints,
      coverageStablePoints: this.coverageStablePoints,
      manualPointCount: this.manualPoints.size,
      manualEdgeCount: this.operatorEdges.size,
      performance: this.performanceProfiler.snapshot(),
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

    clientDiagnostics.info(normalFinish ? 'XR_SCAN_FINISHED' : 'XR_SCAN_INTERRUPTED', {
      stablePoints: snapshot.pointCount,
      tentativePoints: snapshot.tentativePointCount,
      retainedPoints: snapshot.rawPointCount,
      keyframes: snapshot.integrationKeyframes,
      weak: this.coverageWeakPoints,
      improving: this.coverageImprovingPoints,
      stable: this.coverageStablePoints,
      manualPoints: snapshot.manualControlPoints.length,
      operatorEdges: snapshot.operatorEdges.length,
      performance: this.performanceProfiler.snapshot(),
      residualMm: Math.round(snapshot.averageStableResidualMeters * 1000),
    });
    void clientDiagnostics.flush();
  };

  private refreshCoverageGuide(time: number, force = false): void {
    const adaptiveIntervalMs = Math.max(
      COVERAGE_GPU_UPLOAD_INTERVAL_MS,
      this.performanceProfiler.recommendedCoverageIntervalMs(),
    );
    if (!force && time - this.lastCoverageGpuUploadMs < adaptiveIntervalMs) return;
    this.lastCoverageGpuUploadMs = time;
    const started = performance.now();
    const coverage: LiveCoverageSnapshot = this.pointCloud.liveCoverageSnapshot();
    const counts = countCoverageQualityBands(coverage.confidence.subarray(0, coverage.pointCount));
    this.coverageGuideActive = coverage.active && this.coverageRenderer !== null;
    this.coverageVisibleCells = coverage.visibleCoverageCells;
    this.coverageReadyCells = coverage.readyCoverageCells;
    this.coverageTotalCells = coverage.totalCoverageCells;
    this.coverageTargetIndependentViews = coverage.targetFrameHits;
    this.coverageAverageConfidence = coverage.averageCoverageConfidence;
    this.coverageWeakPoints = counts.weak;
    this.coverageImprovingPoints = counts.improving;
    this.coverageStablePoints = counts.stable;

    if (this.coverageRenderer !== null) {
      const renderSnapshot = composeCoverageRenderSnapshot(
        coverage,
        this.manualPoints.xrReferencePositions(),
      );
      if (renderSnapshot.pointCount > 0) this.coverageRenderer.update(renderSnapshot);
      else this.coverageRenderer.clear();
      this.coverageGuideRenderPoints = coverage.pointCount;
    } else {
      this.coverageGuideRenderPoints = 0;
    }
    this.performanceProfiler.record('coverage', performance.now() - started);
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

  private processPendingManualPoint(
    time: number,
    depth: CpuDepthInformationLike,
    view: XrViewLike,
  ): void {
    const request = this.pendingManualPointRequest;
    if (request === null) return;
    this.pendingManualPointRequest = null;

    try {
      const depthMeters = depth.getDepthInMeters(request.u, request.v);
      if (
        !Number.isFinite(depthMeters) ||
        depthMeters < MIN_MANUAL_DEPTH_METERS ||
        depthMeters > this.pointCloud.maxDepth
      ) {
        const message = 'No valid AR depth at that screen point. Aim at a visible surface and try again.';
        this.publish({ manualPointError: message });
        clientDiagnostics.warn('XR_MANUAL_POINT_DEPTH_INVALID', {
          u: request.u,
          v: request.v,
          depthMeters,
        });
        return;
      }

      const geometry = depthGeometry(depth, view);
      const point = projectDepthSampleToProject({
        u: request.u,
        v: request.v,
        depthMeters,
        projectionMatrix: geometry.projectionMatrix,
        worldFromView: geometry.transform.matrix,
      });
      if (point === null) {
        const message = 'Could not project the selected depth sample into scan coordinates.';
        this.publish({ manualPointError: message });
        clientDiagnostics.warn('XR_MANUAL_POINT_PROJECT_FAILED', { u: request.u, v: request.v });
        return;
      }

      let stored: ManualControlPoint | null;
      let snapped = false;
      if (request.pointId === null) {
        const result = this.manualPoints.addOrSnap(point);
        stored = result.point;
        snapped = result.snapped;
      } else {
        stored = this.manualPoints.update(request.pointId, point);
      }
      if (stored === null) {
        this.publish({ manualPointError: 'The selected manual point no longer exists.' });
        return;
      }

      this.publish({ manualPointCount: this.manualPoints.size, manualPointError: null });
      clientDiagnostics.info(
        request.pointId === null
          ? (snapped ? 'XR_MANUAL_POINT_SNAPPED' : 'XR_MANUAL_POINT_ADDED')
          : 'XR_MANUAL_POINT_MOVED',
        {
          id: stored.id,
          snapped,
          x: Number(stored.position[0].toFixed(3)),
          y: Number(stored.position[1].toFixed(3)),
          z: Number(stored.position[2].toFixed(3)),
        },
      );
      this.refreshCoverageGuide(time, true);
    } catch (error) {
      const message = `Manual point depth access failed: ${errorText(error)}`;
      this.publish({ manualPointError: message });
      clientDiagnostics.warn('XR_MANUAL_POINT_FAILED', error);
    }
  }

  private captureDepth(time: number, frame: XrFrameLike, pose: XrViewerPoseLike): void {
    if (this.scanPaused || !this.depthEnabled || pose.views.length === 0) return;
    if (typeof frame.getDepthInformation !== 'function') {
      if (!this.depthFailureReported) {
        this.depthFailureReported = true;
        const message = 'XRFrame.getDepthInformation() is unavailable.';
        clientDiagnostics.error('XR_DEPTH_API_MISSING', message);
        this.publish({ depthError: message });
      }
      return;
    }

    const view = pose.views[0];
    try {
      const depthStarted = performance.now();
      const depth = frame.getDepthInformation(view);
      this.performanceProfiler.record('depthAccess', performance.now() - depthStarted);
      if (depth === null) return;
      this.depthFrameCount += 1;
      this.depthWidth = depth.width;
      this.depthHeight = depth.height;

      if (!this.depthReadyLogged) {
        this.depthReadyLogged = true;
        clientDiagnostics.info('XR_DEPTH_STREAM_READY', {
          width: depth.width,
          height: depth.height,
          usage: this.state.depthUsage,
          format: this.state.depthDataFormat,
          type: this.state.depthType,
        });
        void clientDiagnostics.flush();
      }

      this.processPendingManualPoint(time, depth, view);

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
      const fusionStarted = performance.now();
      const result = this.pointCloud.sampleDepthFrame(
        depth,
        depthGeometry(depth, view),
        context,
      );
      this.performanceProfiler.record('fusion', performance.now() - fusionStarted);
      if (result.changedPoints > 0) this.pointCloudVersion += 1;
      this.refreshCoverageGuide(time);
    } catch (error) {
      if (!this.depthFailureReported) {
        this.depthFailureReported = true;
        const message = `Depth access failed: ${errorText(error)}`;
        clientDiagnostics.error('XR_DEPTH_ACCESS_FAILED', message);
        this.publish({ depthError: message });
      }
    }
  }

  private reportClientTelemetry(time: number): void {
    if (time - this.lastClientTelemetryMs < CLIENT_TELEMETRY_INTERVAL_MS) return;
    this.lastClientTelemetryMs = time;
    clientDiagnostics.info('XR_PROGRESS', {
      frames: this.frameCount,
      depthFrames: this.depthFrameCount,
      integratedFrames: this.sampledDepthFrames,
      stablePoints: this.pointCloud.promotedCount,
      retainedPoints: this.pointCloud.pointCount,
      keyframes: this.pointCloud.keyframeCount,
      polishFrames: this.pointCloud.polishFrameCount,
      readyCells: this.coverageReadyCells,
      visibleCells: this.coverageVisibleCells,
      weak: this.coverageWeakPoints,
      improving: this.coverageImprovingPoints,
      stable: this.coverageStablePoints,
      manualPoints: this.manualPoints.size,
      operatorEdges: this.operatorEdges.size,
      motion: this.motionState,
      performance: this.performanceProfiler.snapshot(),
    });
    void clientDiagnostics.flush();
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
    this.performanceProfiler.recordFrameTimestamp(time);
    const frameWorkStarted = performance.now();
    gl.bindFramebuffer(gl.FRAMEBUFFER, layer.framebuffer);
    gl.viewport(0, 0, layer.framebufferWidth, layer.framebufferHeight);
    gl.clearColor(0, 0, 0, 0);
    gl.clearDepth(1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

    const pose = frame.getViewerPose(referenceSpace);
    if (pose !== null && pose.views.length > 0) {
      const view = pose.views[0];
      for (let index = 0; index < 16; index += 1) {
        this.latestProjectionMatrix[index] = Number(view.projectionMatrix[index]);
        this.latestWorldFromView[index] = Number(view.transform.matrix[index]);
      }
      this.hasLatestProjection = true;
    }
    this.frameCount += 1;
    this.updateLightEstimate(time, frame);

    if (pose !== null && !this.scanPaused) {
      this.captureDepth(time, frame, pose);
      this.coverageRenderer?.render(layer, pose.views);
    }

    this.performanceProfiler.record('frameWork', performance.now() - frameWorkStarted);
    this.reportClientTelemetry(time);

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
      coverageWeakPoints: this.coverageWeakPoints,
      coverageImprovingPoints: this.coverageImprovingPoints,
      coverageStablePoints: this.coverageStablePoints,
      coverageGuideError: this.coverageGuideError,
      manualPointCount: this.manualPoints.size,
      manualEdgeCount: this.operatorEdges.size,
      performance: this.performanceProfiler.snapshot(),
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
