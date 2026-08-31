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
}

interface XrViewLike {}
interface XrReferenceSpaceLike {}

interface XrViewerPoseLike {
  transform: XrRigidTransformLike;
  views: readonly XrViewLike[];
}

interface XrFrameLike {
  session: XrSessionLike;
  getViewerPose(referenceSpace: XrReferenceSpaceLike): XrViewerPoseLike | null;
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
}

interface XrSessionInitLike {
  optionalFeatures?: string[];
  domOverlay?: {
    root: Element;
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

const initialRuntimeState: WebXrRuntimeState = {
  phase: 'idle',
  referenceSpace: null,
  domOverlay: false,
  environmentBlendMode: null,
  frameCount: 0,
  tracking: false,
  pose: null,
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

function hasWebGl(): boolean {
  const context = createWebGlContext();
  if (context === null) {
    return false;
  }

  const extension = context.getExtension('WEBGL_lose_context');
  extension?.loseContext();
  return true;
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

    this.publish({
      phase: 'starting',
      referenceSpace: null,
      domOverlay: false,
      environmentBlendMode: null,
      frameCount: 0,
      tracking: false,
      pose: null,
      error: null,
    });

    let session: XrSessionLike | null = null;

    try {
      session = await xr.requestSession('immersive-ar', {
        optionalFeatures: ['local-floor', 'dom-overlay'],
        domOverlay: {
          root: document.body,
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

      document.documentElement.classList.add('xr-session-active');

      this.publish({
        phase: 'running',
        referenceSpace: referenceSpaceType,
        domOverlay: session.domOverlayState != null,
        environmentBlendMode: session.environmentBlendMode ?? null,
        frameCount: 0,
        tracking: false,
        pose: null,
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
        error: error instanceof Error ? error.message : String(error),
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
        error: error instanceof Error ? error.message : String(error),
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

  private cleanupRuntime(): void {
    document.documentElement.classList.remove('xr-session-active');
    this.session = null;
    this.referenceSpace = null;
    this.gl = null;
    this.layer = null;
    this.frameCount = 0;
    this.lastUiUpdateMs = 0;
  }

  private readonly handleSessionEnd = (): void => {
    this.cleanupRuntime();
    this.publish({
      phase: 'ended',
      referenceSpace: null,
      domOverlay: false,
      environmentBlendMode: null,
      tracking: false,
      pose: null,
      error: null,
    });
  };

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

    if (time - this.lastUiUpdateMs < 100 && this.frameCount > 1) {
      return;
    }

    this.lastUiUpdateMs = time;

    if (pose === null) {
      this.publish({
        frameCount: this.frameCount,
        tracking: false,
        pose: null,
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
    });
  };
}
