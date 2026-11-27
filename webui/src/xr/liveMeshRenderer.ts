import { captureWebGlDrawState, restoreWebGlDrawState } from './webGlStateGuard';
import {
  compileShader, isWebGl2, multiplyMat4, rigidInverse,
  type CoverageRenderLayer, type CoverageRenderView,
} from './liveCoverageRenderer';

type GlContext = WebGLRenderingContext | WebGL2RenderingContext;

/**
 * Opacity of the live mesh. The XR layer's framebuffer is transparent and the compositor puts it over the camera (WebXR AR module), so the renderer
 * writes the premultiplied pixel (colour x alpha, alpha) DIRECTLY, without blending: a pixel covered by one triangle or by a thousand shows exactly this
 * opacity. Blending here would also blend the alpha channel and leave about alpha squared on a transparent buffer.
 */
export const LIVE_MESH_ALPHA = 0.10;
/** The most opaque the meshes may make any pixel; with direct writes it is simply the larger of their alphas. */
export const MESH_OVERLAY_OPACITY_BUDGET = 0.25;

export interface LiveMeshStyle { color: readonly [number, number, number]; alpha: number }
/** The mesh being built now: cool blue. */
export const LIVE_MESH_STYLE: LiveMeshStyle = { color: [0.30, 0.78, 1.0], alpha: LIVE_MESH_ALPHA };
/** The mesh of the saved scan drawn over a Continue: warm amber, fainter, so it reads as "already there" next to the blue one being built. */
export const REFERENCE_MESH_STYLE: LiveMeshStyle = { color: [1.0, 0.72, 0.28], alpha: 0.08 };

export interface LiveMeshRendererOptions { style?: LiveMeshStyle }

function createProgram(gl: GlContext, style: LiveMeshStyle): WebGLProgram {
  const webGl2 = isWebGl2(gl);
  const premultiplied = `vec4(${[...style.color.map((v) => v * style.alpha), style.alpha].map((v) => v.toFixed(5)).join(', ')})`;
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
      void main() { out_color = ${premultiplied}; }`
    : `precision mediump float;
      void main() { gl_FragColor = ${premultiplied}; }`;
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
 * Translucent triangles over the camera. Each covered pixel is written once with the mesh's premultiplied value, whatever overlaps there; draw order
 * decides between two meshes (the saved scan's first, the live one over it; the coverage guide is drawn after both). The depth buffer is tested, so
 * anything that put real depth there still occludes the mesh, and is never written, so the mesh occludes nothing. Double-sided: the live normals are not
 * trusted to face the camera.
 */
export class LiveMeshRenderer {
  private readonly program: WebGLProgram;
  private readonly buffer: WebGLBuffer;
  private readonly positionLocation: number;
  private readonly matrixLocation: WebGLUniformLocation;
  private readonly viewMatrix = new Float32Array(16);
  private readonly viewProjection = new Float32Array(16);
  private vertexCount = 0;

  readonly style: LiveMeshStyle;

  constructor(private readonly gl: GlContext, options: LiveMeshRendererOptions = {}) {
    this.style = options.style ?? LIVE_MESH_STYLE;
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
    /* Drawn in EVERY XR frame, whatever the pressure: the framebuffer is cleared each frame, so a skipped frame would show no mesh and flicker. */
    if (this.vertexCount <= 0) return;
    const gl = this.gl;
    const previous = captureWebGlDrawState(gl, [this.positionLocation]);
    try {
      gl.useProgram(this.program);
      gl.disable(gl.BLEND);
      gl.enable(gl.DEPTH_TEST); gl.depthFunc(gl.LEQUAL); gl.depthMask(false);
      gl.colorMask(true, true, true, true);
      gl.disable(gl.STENCIL_TEST);
      gl.disable(gl.CULL_FACE);
      gl.bindBuffer(gl.ARRAY_BUFFER, this.buffer);
      gl.enableVertexAttribArray(this.positionLocation);
      gl.vertexAttribPointer(this.positionLocation, 3, gl.FLOAT, false, 0, 0);
      for (const view of views) {
        const viewport = layer.getViewport(view);
        gl.viewport(viewport.x, viewport.y, viewport.width, viewport.height);
        if (!rigidInverse(view.transform.matrix, this.viewMatrix)) continue;
        if (!multiplyMat4(view.projectionMatrix, this.viewMatrix, this.viewProjection)) continue;
        gl.uniformMatrix4fv(this.matrixLocation, false, this.viewProjection);
        gl.drawArrays(gl.TRIANGLES, 0, this.vertexCount);
      }
    } finally { restoreWebGlDrawState(gl, previous); }
  }

  dispose(): void {
    this.gl.deleteBuffer(this.buffer);
    this.gl.deleteProgram(this.program);
    this.vertexCount = 0;
  }
}
