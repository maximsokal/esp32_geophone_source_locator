import {
  DepthPointCloudAccumulator,
  type CpuDepthInformationLike,
  type DepthSampleView,
  type ScanPointCloudSnapshot,
} from './depthPointCloud';

export type XrRuntimePhase =
  | 'idle'
  | 'starting'
  | 'running'
  | 'stopping'
  | 'ended'
  | 'error';

export interface WebXrCapabilities {
  secureContext: boolean;
  cameraApi: boolean;
  webGl: boolean;
  xrApi: boolean;
  immersiveAr: boolean;
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
  depthAvailable: boolean;
  depthUsage: string | null;
  depthDataFormat: string | null;
  depthWidth: number | null;
  depthHeight: number | null;
  depthFrameCount: number;
  sampledDepthFrames: number;
  pointCount: number;
  pointCloudVersion: number;
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

interface XrViewLike extends DepthSampleView {
  projectionMatrix: ArrayLike<number>;
  transform: XrRigidTransformLike;
}

interface XrReferenceSpaceLike {}

interface XrViewerPoseLike {
  transform: XrRigidTransformLike;
  views: readonly XrViewLike[];
}

interface XrFrameLike {
  session: XrSessionLike;
  getViewerPose(referenceSpace: XrReferenceSpaceLike): XrViewerPoseLike | null;
  getDepthInformation?(view: XrViewLike): CpuDepthInformationLike | null;
}

type XrFrameCallback = (time: number, frame: XrFrameLike) => void;

interface XrWebGlLayerLike {
  framebuffer: WebGLFramebuffer | null;
  framebufferWidth: number;
  framebufferHeight: number;
}

interface XrSessionLike extends EventTarget {
  requestReferenceSpace(type: 'local-floor' | 'local'): Promise<XrReferenceSpaceLike>;
  requestAnimationFrame(callback: XrFrameCallback): number;
  updateRenderState(state: { baseLayer: XrWebGlLayerLike }): void | Promise<void>;
  end(): Promise<void>;
  domOverlayState?: { type: string } | null;
  environmentBlendMode?: string;
  depthUsage?: string;
  depthDataFormat?: string;
}

interface XrSessionInitLike {
  optionalFeatures?: string[];
  domOverlay?: {
    root: Element;
  };
  depthSensing?: {
    usagePreference: string[];
    dataFormatPreference: string[];
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

type NavigatorWithXr = Navigator & {
  xr?: XrSystemLike;
};

type WindowWithXrLayer = Window & {
  XRWebGLLayer?: XrWebGlLayerConstructor;
};

type RuntimeListener = (state: WebXrRuntimeState) => void;

interface DepthConfiguration {
  enabled: boolean;
  usage: string | null;
  dataFormat: string | null;
}

const DEPTH_SAMPLE_INTERVAL_MS = 140;

const initialRuntimeState: WebXrRuntimeState = {
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
  if (webGl2 !== null) {
    return webGl2 as XrCompatibleGl;
  }

  const webGl = canvas.getContext('webgl', attributes);
  return webGl === null ? null : (webGl as XrCompatibleGl);
}

function releaseWebGlContext(context: XrCompatibleGl | null): void {
  context?.getExtension('WEBGL_lose_context')?.loseContext();
}

function hasWebGl(): boolean {
  const context = createWebGlContext();
  if (context === null) {
    return false;
  }

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

    return {
      enabled: usage === 'cpu-optimized',
      usage,
      dataFormat,
    };
  } catch {
    // The current depth specification requires these getters to throw when
    // depth-sensing was not enabled on the session. Optional feature denial
    // must therefore be treated as a normal capability result, not a scan
    // startup failure.
    return {
      enabled: false,
      usage: null,
      dataFormat: null,
    };
  }
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
  };
}

export class WebXrRuntime {
  private state: WebXrRuntimeState = initialRuntimeState;
  private listeners = new Set<RuntimeListener>();
  private session: XrSessionLike | null = null;
  private referenceSpace: XrReferenceSpaceLike | null = null;
  private gl: XrCompatibleGl | null = null;
  private layer: XrWebGlLayerLike | null = null;
  private frameCount = 0;
  private lastUiUpdateMs = 0;
  private lastDepthSampleMs = Number.NEGATIVE_INFINITY;
  private depthFrameCount = 0;
  private sampledDepthFrames = 0;
  private pointCloudVersion = 0;
  private depthWidth: number | null = null;
  private depthHeight: number | null = null;
  private depthFailureReported = false;
  private depthEnabled = false;
  private readonly pointCloud = new DepthPointCloudAccumulator();

  subscribe(listener: RuntimeListener): () => void {
    this.listeners.add(listener);
    listener(this.state);

    return () => {
      this.listeners.delete(listener);
    };
  }

  getState(): WebXrRuntimeState {
    return this.state;
  }

  getPointCloudSnapshot(): ScanPointCloudSnapshot {
    return this.pointCloud.snapshot(this.depthFrameCount, this.sampledDepthFrames);
  }

  async start(): Promise<void> {
    if (this.session !== null || this.state.phase === 'starting') {
      return;
    }

    const xr = (navigator as NavigatorWithXr).xr;
    const Layer = (window as WindowWithXrLayer).XRWebGLLayer;

    if (!window.isSecureContext) {
      this.publish({
        phase: 'error',
        error: 'WebXR immersive AR requires a trusted HTTPS secure context.',
      });
      return;
    }

    if (xr === undefined) {
      this.publish({
        phase: 'error',
        error: 'navigator.xr is not available in this browser.',
      });
      return;
    }

    if (Layer === undefined) {
      this.publish({
        phase: 'error',
        error: 'XRWebGLLayer is not available in this browser.',
      });
      return;
    }

    this.resetScanState();

    this.publish({
      phase: 'starting',
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
    });

    let session: XrSessionLike | null = null;

    try {
      session = await xr.requestSession('immersive-ar', {
        optionalFeatures: ['local-floor', 'dom-overlay', 'depth-sensing'],
        domOverlay: {
          root: document.body,
        },
        depthSensing: {
          usagePreference: ['cpu-optimized'],
          dataFormatPreference: ['luminance-alpha', 'float32'],
        },
      });

      this.session = session;
      session.addEventListener('end', this.handleSessionEnd);

      const gl = createWebGlContext();
      if (gl === null) {
        throw new Error('Unable to create a WebGL context for WebXR.');
      }

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
      this.frameCount = 0;
      this.lastUiUpdateMs = 0;
      this.lastDepthSampleMs = Number.NEGATIVE_INFINITY;

      document.documentElement.classList.add('xr-session-active');

      const depthConfiguration = readDepthConfiguration(session);
      this.depthEnabled = depthConfiguration.enabled;

      this.publish({
        phase: 'running',
        referenceSpace: referenceSpaceType,
        domOverlay: session.domOverlayState != null,
        environmentBlendMode: session.environmentBlendMode ?? null,
        frameCount: 0,
        tracking: false,
        pose: null,
        depthAvailable: depthConfiguration.enabled,
        depthUsage: depthConfiguration.usage,
        depthDataFormat: depthConfiguration.dataFormat,
        depthWidth: null,
        depthHeight: null,
        depthFrameCount: 0,
        sampledDepthFrames: 0,
        pointCount: 0,
        pointCloudVersion: 0,
        depthError: depthConfiguration.enabled
          ? null
          : 'AR tracking is available, but this browser/device did not enable WebXR depth-sensing for the session.',
        error: null,
      });

      session.requestAnimationFrame(this.handleFrame);
    } catch (error) {
      if (session !== null) {
        session.removeEventListener('end', this.handleSessionEnd);
        try {
          await session.end();
        } catch {
          // Best-effort cleanup after a failed session start.
        }
      }

      this.cleanupRuntime();
      this.publish({
        phase: 'error',
        referenceSpace: null,
        domOverlay: false,
        environmentBlendMode: null,
        tracking: false,
        pose: null,
        depthAvailable: false,
        depthUsage: null,
        depthDataFormat: null,
        depthError: null,
        error: errorText(error),
      });
    }
  }

  async stop(): Promise<void> {
    const session = this.session;
    if (session === null) {
      return;
    }

    this.publish({
      phase: 'stopping',
      error: null,
    });

    try {
      await session.end();
    } catch (error) {
      this.publish({
        phase: 'error',
        error: errorText(error),
      });
    }
  }

  async dispose(): Promise<void> {
    const session = this.session;

    if (session !== null) {
      session.removeEventListener('end', this.handleSessionEnd);
      try {
        await session.end();
      } catch {
        // The browser may already have ended the immersive session.
      }
    }

    this.cleanupRuntime();
    this.listeners.clear();
  }

  private publish(patch: Partial<WebXrRuntimeState>): void {
    this.state = {
      ...this.state,
      ...patch,
    };

    for (const listener of this.listeners) {
      listener(this.state);
    }
  }

  private resetScanState(): void {
    this.pointCloud.reset();
    this.depthFrameCount = 0;
    this.sampledDepthFrames = 0;
    this.pointCloudVersion = 0;
    this.depthWidth = null;
    this.depthHeight = null;
    this.depthFailureReported = false;
    this.depthEnabled = false;
    this.lastDepthSampleMs = Number.NEGATIVE_INFINITY;
  }

  private cleanupRuntime(): void {
    document.documentElement.classList.remove('xr-session-active');
    releaseWebGlContext(this.gl);
    this.session = null;
    this.referenceSpace = null;
    this.gl = null;
    this.layer = null;
    this.frameCount = 0;
    this.lastUiUpdateMs = 0;
    this.lastDepthSampleMs = Number.NEGATIVE_INFINITY;
    this.depthEnabled = false;
  }

  private readonly handleSessionEnd = (): void => {
    const finalPointCount = this.pointCloud.pointCount;
    const finalDepthFrames = this.depthFrameCount;
    const finalSampledFrames = this.sampledDepthFrames;
    const finalVersion = this.pointCloudVersion;

    this.cleanupRuntime();
    this.publish({
      phase: 'ended',
      referenceSpace: null,
      domOverlay: false,
      environmentBlendMode: null,
      tracking: false,
      pose: null,
      depthFrameCount: finalDepthFrames,
      sampledDepthFrames: finalSampledFrames,
      pointCount: finalPointCount,
      pointCloudVersion: finalVersion,
      error: null,
    });
  };

  private captureDepth(time: number, frame: XrFrameLike, pose: XrViewerPoseLike): void {
    if (!this.depthEnabled || pose.views.length === 0) {
      return;
    }

    if (typeof frame.getDepthInformation !== 'function') {
      if (!this.depthFailureReported) {
        this.depthFailureReported = true;
        this.publish({
          depthError: 'Depth was enabled on the XR session, but XRFrame.getDepthInformation() is unavailable.',
        });
      }
      return;
    }

    const view = pose.views[0];

    try {
      const depth = frame.getDepthInformation(view);
      if (depth === null) {
        return;
      }

      this.depthFrameCount += 1;
      this.depthWidth = depth.width;
      this.depthHeight = depth.height;

      if (time - this.lastDepthSampleMs < DEPTH_SAMPLE_INTERVAL_MS) {
        return;
      }

      this.lastDepthSampleMs = time;
      this.sampledDepthFrames += 1;

      const added = this.pointCloud.sampleDepthFrame(depth, view);
      if (added > 0) {
        this.pointCloudVersion += 1;
      }
    } catch (error) {
      if (!this.depthFailureReported) {
        this.depthFailureReported = true;
        this.publish({
          depthError: `Depth frame access failed: ${errorText(error)}`,
        });
      }
    }
  }

  private readonly handleFrame: XrFrameCallback = (time, frame): void => {
    const session = this.session;
    const referenceSpace = this.referenceSpace;
    const gl = this.gl;
    const layer = this.layer;

    if (
      session === null ||
      referenceSpace === null ||
      gl === null ||
      layer === null ||
      frame.session !== session
    ) {
      return;
    }

    session.requestAnimationFrame(this.handleFrame);

    gl.bindFramebuffer(gl.FRAMEBUFFER, layer.framebuffer);
    gl.viewport(0, 0, layer.framebufferWidth, layer.framebufferHeight);
    gl.clearColor(0, 0, 0, 0);
    gl.clearDepth(1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

    const pose = frame.getViewerPose(referenceSpace);
    this.frameCount += 1;

    if (pose !== null) {
      this.captureDepth(time, frame, pose);
    }

    if (time - this.lastUiUpdateMs < 100 && this.frameCount > 1) {
      return;
    }

    this.lastUiUpdateMs = time;

    if (pose === null) {
      this.publish({
        frameCount: this.frameCount,
        tracking: false,
        pose: null,
        depthWidth: this.depthWidth,
        depthHeight: this.depthHeight,
        depthFrameCount: this.depthFrameCount,
        sampledDepthFrames: this.sampledDepthFrames,
        pointCount: this.pointCloud.pointCount,
        pointCloudVersion: this.pointCloudVersion,
      });
      return;
    }

    const { position, orientation } = pose.transform;

    this.publish({
      frameCount: this.frameCount,
      tracking: true,
      pose: {
        position: [position.x, position.y, position.z],
        orientation: [
          orientation.x,
          orientation.y,
          orientation.z,
          orientation.w,
        ],
        views: pose.views.length,
      },
      depthWidth: this.depthWidth,
      depthHeight: this.depthHeight,
      depthFrameCount: this.depthFrameCount,
      sampledDepthFrames: this.sampledDepthFrames,
      pointCount: this.pointCloud.pointCount,
      pointCloudVersion: this.pointCloudVersion,
    });
  };
}

export type { ScanPointCloudSnapshot } from './depthPointCloud';
