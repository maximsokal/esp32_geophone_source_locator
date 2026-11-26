import { activeXrRenderPolicy } from './xrPerformanceProfiler';
import { captureWebGlDrawState, restoreWebGlDrawState } from './webGlStateGuard';
import {
  compileShader, isWebGl2, multiplyMat4, rigidInverse,
  type CoverageRenderLayer, type CoverageRenderView,
} from './liveCoverageRenderer';

type GlContext = WebGLRenderingContext | WebGL2RenderingContext;

/**
 * Fill of the translucent live mesh. With the single-layer pass below this IS the opacity of the mesh at every pixel, however many shells overlap there;
 * 0.10 keeps the camera image, texture and AprilTags readable (phone run 2026-11-26: 0.30 per layer summed to 51% for two layers and 94% for eight).
 */
export const LIVE_MESH_ALPHA = 0.10;
/** The most opaque the meshes may make any pixel when the saved scan's mesh (amber) and the live mesh (blue) are both drawn. */
export const MESH_OVERLAY_OPACITY_BUDGET = 0.25;

export interface LiveMeshStyle { color: readonly [number, number, number]; alpha: number }
/** The mesh being built now: cool blue. */
export const LIVE_MESH_STYLE: LiveMeshStyle = { color: [0.30, 0.78, 1.0], alpha: LIVE_MESH_ALPHA };
/** The mesh of the saved scan drawn over a Continue: warm amber, fainter, so it reads as "already there" next to the blue one being built. */
export const REFERENCE_MESH_STYLE: LiveMeshStyle = { color: [1.0, 0.72, 0.28], alpha: 0.08 };

/** 'single-layer': a depth-only pass, then ONE blended fragment per pixel (the nearest). 'accumulate': every shell blends in (the old look; kept for comparison tests). */
export type LiveMeshBlendMode = 'single-layer' | 'accumulate';
export interface LiveMeshRendererOptions { style?: LiveMeshStyle; mode?: LiveMeshBlendMode; /** Push this mesh slightly back so that it yields to a coincident mesh drawn before it. */ depthBias?: boolean }

function createProgram(gl: GlContext, style: LiveMeshStyle): WebGLProgram {
  const webGl2 = isWebGl2(gl);
  const body = `vec3 color = vec3(${style.color.map((v) => v.toFixed(3)).join(', ')}); float alpha = ${style.alpha.toFixed(3)};`;
  const vertexSource = webGl2
    ? `#version 300 es
      precision highp float; in vec3 a_position; uniform mat4 u_view_projection;
      invariant gl_Position;
      void main() { gl_Position = u_view_projection * vec4(a_position, 1.0); }`
    : `precision highp float; attribute vec3 a_position; uniform mat4 u_view_projection;
      invariant gl_Position;
      void main() { gl_Position = u_view_projection * vec4(a_position, 1.0); }`;
  const fragmentSource = webGl2
    ? `#version 300 es
      precision mediump float; out vec4 out_color;
      void main() { ${body} out_color = vec4(color, alpha); }`
    : `precision mediump float;
      void main() { ${body} gl_FragColor = vec4(color, alpha); }`;
  const vertex = compileShader(gl, gl.VERTEX_SHADER, vertexSource);
  const fragment = compileShader(gl, gl.FRAGMENT_SHADER, fragmentSource);
  const program = gl.createProgram();
  if (program === null) { gl.deleteShader(vertex); gl.deleteShader(fragment); throw new Error('Unable to allocate live-mesh WebGL program.'); }
  gl.attachShader(program, vertex); gl.attachShader(program, fragment); gl.linkProgram(program);
  gl.deleteShader(vertex); gl.deleteShader(fragment);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    const log = gl.getProgramInfoLog(program) ?? 'unknown link error';
    gl.deleteProgram(program);
    throw new Error(`Live-mesh program link failed: ${log}`);
  }
  return program;
}

/**
 * Translucent triangles over the camera: depth-tested against the real world (so the mesh hides behind real surfaces), never writing depth, drawn
 * after the point guide. Double-sided: the live normals are not trusted to face the camera.
 */
export class LiveMeshRenderer {
  private readonly program: WebGLProgram;
  private readonly buffer: WebGLBuffer;
  private readonly positionLocation: number;
  private readonly matrixLocation: WebGLUniformLocation;
  private readonly viewMatrix = new Float32Array(16);
  private readonly viewProjection = new Float32Array(16);
  private vertexCount = 0;
  private lastDrawMs = Number.NEGATIVE_INFINITY;

  readonly style: LiveMeshStyle;
  readonly mode: LiveMeshBlendMode;
  private readonly depthBias: boolean;

  constructor(private readonly gl: GlContext, options: LiveMeshRendererOptions = {}) {
    this.style = options.style ?? LIVE_MESH_STYLE;
    this.mode = options.mode ?? 'single-layer';
    this.depthBias = options.depthBias ?? false;
    this.program = createProgram(gl, this.style);
    const buffer = gl.createBuffer();
    const matrix = gl.getUniformLocation(this.program, 'u_view_projection');
    this.positionLocation = gl.getAttribLocation(this.program, 'a_position');
    if (buffer === null || matrix === null || this.positionLocation < 0) {
      if (buffer !== null) gl.deleteBuffer(buffer);
      gl.deleteProgram(this.program);
      throw new Error('Live-mesh WebGL resources are unavailable.');
    }
    this.buffer = buffer;
    this.matrixLocation = matrix;
  }

  get triangleCount(): number { return this.vertexCount / 3; }

  update(trianglesXr: Float32Array, triangleCount: number): void {
    this.vertexCount = triangleCount * 3;
    const gl = this.gl;
    const previous = gl.getParameter(gl.ARRAY_BUFFER_BINDING) as WebGLBuffer | null;
    try {
      gl.bindBuffer(gl.ARRAY_BUFFER, this.buffer);
      gl.bufferData(gl.ARRAY_BUFFER, trianglesXr.subarray(0, this.vertexCount * 3), gl.DYNAMIC_DRAW);
    } finally { gl.bindBuffer(gl.ARRAY_BUFFER, previous); }
  }

  clear(): void { this.vertexCount = 0; }

  render(layer: CoverageRenderLayer, views: readonly CoverageRenderView[]): void {
    if (this.vertexCount <= 0) return;
    const policy = activeXrRenderPolicy();
    const now = performance.now();
    if (policy.drawIntervalMs > 0 && now - this.lastDrawMs < policy.drawIntervalMs) return;
    this.lastDrawMs = now;
    const gl = this.gl;
    const previous = captureWebGlDrawState(gl, [this.positionLocation]);
    try {
      const offsetWasOn = gl.isEnabled(gl.POLYGON_OFFSET_FILL);
      const offsetFactor = gl.getParameter(gl.POLYGON_OFFSET_FACTOR) as number;
      const offsetUnits = gl.getParameter(gl.POLYGON_OFFSET_UNITS) as number;
      gl.useProgram(this.program);
      gl.enable(gl.BLEND); gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
      gl.enable(gl.DEPTH_TEST);
      gl.disable(gl.CULL_FACE);
      if (this.depthBias) { gl.enable(gl.POLYGON_OFFSET_FILL); gl.polygonOffset(1, 1); }
      gl.bindBuffer(gl.ARRAY_BUFFER, this.buffer);
      gl.enableVertexAttribArray(this.positionLocation);
      gl.vertexAttribPointer(this.positionLocation, 3, gl.FLOAT, false, 0, 0);
      for (const view of views) {
        const viewport = layer.getViewport(view);
        gl.viewport(viewport.x, viewport.y, viewport.width, viewport.height);
        if (!rigidInverse(view.transform.matrix, this.viewMatrix)) continue;
        if (!multiplyMat4(view.projectionMatrix, this.viewMatrix, this.viewProjection)) continue;
        gl.uniformMatrix4fv(this.matrixLocation, false, this.viewProjection);
        if (this.mode === 'single-layer') {
          /* Pass 1: depth only, against whatever is already there (LESS), so the nearest shell of this mesh is the only one left standing at each pixel. */
          gl.colorMask(false, false, false, false); gl.depthMask(true); gl.depthFunc(gl.LESS);
          gl.drawArrays(gl.TRIANGLES, 0, this.vertexCount);
          /* Pass 2: colour only where the depth equals that nearest shell: one blended fragment per pixel, so overlap cannot add up. */
          gl.colorMask(true, true, true, true); gl.depthMask(false); gl.depthFunc(gl.EQUAL);
          gl.drawArrays(gl.TRIANGLES, 0, this.vertexCount);
        } else {
          gl.depthMask(false); gl.depthFunc(gl.LEQUAL);
          gl.drawArrays(gl.TRIANGLES, 0, this.vertexCount);
        }
      }
      if (this.depthBias) {
        if (!offsetWasOn) gl.disable(gl.POLYGON_OFFSET_FILL);
        gl.polygonOffset(offsetFactor, offsetUnits);
      }
    } finally { restoreWebGlDrawState(gl, previous); }
  }

  dispose(): void {
    this.gl.deleteBuffer(this.buffer);
    this.gl.deleteProgram(this.program);
    this.vertexCount = 0;
  }
}
