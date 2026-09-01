import { activeXrRenderPolicy, recordActiveXrPerformanceMetric } from './xrPerformanceProfiler';

export interface CoverageRenderView {
  projectionMatrix: ArrayLike<number>;
  transform: {
    matrix: ArrayLike<number>;
  };
}

export interface CoverageRenderViewport {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface CoverageRenderLayer {
  framebuffer: WebGLFramebuffer | null;
  getViewport(view: CoverageRenderView): CoverageRenderViewport;
}

export interface CoverageRenderSnapshot {
  /** Positions in the active WebXR reference-space coordinate system. */
  positions: Float32Array;
  /** Confidence in [0,1]. Values >1.5 are reserved for operator pins. */
  confidence: Float32Array;
  pointCount: number;
  /** Manual pins are appended at the end; optional for older snapshot producers. */
  manualPointCount?: number;
}

type GlContext = WebGLRenderingContext | WebGL2RenderingContext;

function isWebGl2(gl: GlContext): gl is WebGL2RenderingContext {
  return typeof WebGL2RenderingContext !== 'undefined' && gl instanceof WebGL2RenderingContext;
}

function compileShader(gl: GlContext, type: number, source: string): WebGLShader {
  const shader = gl.createShader(type);
  if (shader === null) throw new Error('Unable to allocate coverage-guide shader.');
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const log = gl.getShaderInfoLog(shader) ?? 'unknown shader compile error';
    gl.deleteShader(shader);
    throw new Error(`Coverage-guide shader compile failed: ${log}`);
  }
  return shader;
}

function createProgram(gl: GlContext): WebGLProgram {
  const webGl2 = isWebGl2(gl);
  const vertexSource = webGl2
    ? `#version 300 es
      precision highp float;
      in vec3 a_position;
      in float a_confidence;
      uniform mat4 u_view_projection;
      out float v_confidence;
      void main() {
        gl_Position = u_view_projection * vec4(a_position, 1.0);
        bool manual = a_confidence > 1.5;
        float quality = clamp(a_confidence, 0.0, 1.0);
        gl_PointSize = manual ? 24.0 : mix(4.0, 7.5, quality);
        v_confidence = a_confidence;
      }
    `
    : `
      precision highp float;
      attribute vec3 a_position;
      attribute float a_confidence;
      uniform mat4 u_view_projection;
      varying float v_confidence;
      void main() {
        gl_Position = u_view_projection * vec4(a_position, 1.0);
        bool manual = a_confidence > 1.5;
        float quality = clamp(a_confidence, 0.0, 1.0);
        gl_PointSize = manual ? 24.0 : mix(4.0, 7.5, quality);
        v_confidence = a_confidence;
      }
    `;

  const fragmentBody = `
      float radius = distance(gl_PointCoord, vec2(0.5));
      if (radius > 0.5) discard;
      bool manual = v_confidence > 1.5;
      float t = clamp(v_confidence, 0.0, 1.0);
      vec3 coverageColor = t < 0.42
        ? vec3(1.00, 0.20, 0.10)
        : (t < 0.72 ? vec3(1.00, 0.82, 0.08) : vec3(0.12, 1.00, 0.55));
      vec3 manualColor = radius < 0.13 ? vec3(1.0) : vec3(0.95, 0.18, 1.00);
      vec3 color = manual ? manualColor : coverageColor;
      float edge = 1.0 - smoothstep(0.32, 0.50, radius);
      float alpha = manual ? max(0.86, edge) : mix(0.48, 0.96, t) * edge;
  `;

  const fragmentSource = webGl2
    ? `#version 300 es
      precision mediump float;
      in float v_confidence;
      out vec4 out_color;
      void main() {
        ${fragmentBody}
        out_color = vec4(color, alpha);
      }
    `
    : `
      precision mediump float;
      varying float v_confidence;
      void main() {
        ${fragmentBody}
        gl_FragColor = vec4(color, alpha);
      }
    `;

  const vertex = compileShader(gl, gl.VERTEX_SHADER, vertexSource);
  const fragment = compileShader(gl, gl.FRAGMENT_SHADER, fragmentSource);
  const program = gl.createProgram();
  if (program === null) {
    gl.deleteShader(vertex);
    gl.deleteShader(fragment);
    throw new Error('Unable to allocate coverage-guide WebGL program.');
  }
  gl.attachShader(program, vertex);
  gl.attachShader(program, fragment);
  gl.linkProgram(program);
  gl.deleteShader(vertex);
  gl.deleteShader(fragment);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    const log = gl.getProgramInfoLog(program) ?? 'unknown program link error';
    gl.deleteProgram(program);
    throw new Error(`Coverage-guide program link failed: ${log}`);
  }
  return program;
}

function rigidInverse(matrix: ArrayLike<number>, out: Float32Array): boolean {
  if (matrix.length < 16) return false;
  const m0 = Number(matrix[0]);
  const m1 = Number(matrix[1]);
  const m2 = Number(matrix[2]);
  const m4 = Number(matrix[4]);
  const m5 = Number(matrix[5]);
  const m6 = Number(matrix[6]);
  const m8 = Number(matrix[8]);
  const m9 = Number(matrix[9]);
  const m10 = Number(matrix[10]);
  const tx = Number(matrix[12]);
  const ty = Number(matrix[13]);
  const tz = Number(matrix[14]);
  if (![m0, m1, m2, m4, m5, m6, m8, m9, m10, tx, ty, tz].every(Number.isFinite)) return false;

  out[0] = m0; out[1] = m4; out[2] = m8; out[3] = 0;
  out[4] = m1; out[5] = m5; out[6] = m9; out[7] = 0;
  out[8] = m2; out[9] = m6; out[10] = m10; out[11] = 0;
  out[12] = -(m0 * tx + m1 * ty + m2 * tz);
  out[13] = -(m4 * tx + m5 * ty + m6 * tz);
  out[14] = -(m8 * tx + m9 * ty + m10 * tz);
  out[15] = 1;
  return true;
}

function multiplyMat4(left: ArrayLike<number>, right: ArrayLike<number>, out: Float32Array): boolean {
  if (left.length < 16 || right.length < 16) return false;
  for (let column = 0; column < 4; column += 1) {
    const r0 = Number(right[column * 4]);
    const r1 = Number(right[column * 4 + 1]);
    const r2 = Number(right[column * 4 + 2]);
    const r3 = Number(right[column * 4 + 3]);
    out[column * 4] = Number(left[0]) * r0 + Number(left[4]) * r1 + Number(left[8]) * r2 + Number(left[12]) * r3;
    out[column * 4 + 1] = Number(left[1]) * r0 + Number(left[5]) * r1 + Number(left[9]) * r2 + Number(left[13]) * r3;
    out[column * 4 + 2] = Number(left[2]) * r0 + Number(left[6]) * r1 + Number(left[10]) * r2 + Number(left[14]) * r3;
    out[column * 4 + 3] = Number(left[3]) * r0 + Number(left[7]) * r1 + Number(left[11]) * r2 + Number(left[15]) * r3;
  }
  return true;
}

function inferManualTail(confidence: Float32Array, count: number): number {
  let manual = 0;
  for (let index = count - 1; index >= 0; index -= 1) {
    if (confidence[index] <= 1.5) break;
    manual += 1;
  }
  return manual;
}

/**
 * Deterministic display-only LOD. It never mutates the fusion snapshot and
 * always preserves operator pins at the tail of the render buffer.
 */
export function applyCoverageRenderBudget(snapshot: CoverageRenderSnapshot, regularPointBudget: number): CoverageRenderSnapshot {
  const count = Math.min(snapshot.pointCount, Math.floor(snapshot.positions.length / 3), snapshot.confidence.length);
  const inferredManual = inferManualTail(snapshot.confidence, count);
  const manualPointCount = Math.min(Math.max(0, snapshot.manualPointCount ?? inferredManual), count);
  const regularCount = count - manualPointCount;
  const budget = Math.max(0, Math.floor(regularPointBudget));
  if (regularCount <= budget) {
    return {
      positions: snapshot.positions.subarray(0, count * 3),
      confidence: snapshot.confidence.subarray(0, count),
      pointCount: count,
      manualPointCount,
    };
  }

  const takeRegular = budget;
  const outputCount = takeRegular + manualPointCount;
  const positions = new Float32Array(outputCount * 3);
  const confidence = new Float32Array(outputCount);
  for (let index = 0; index < takeRegular; index += 1) {
    const source = Math.min(regularCount - 1, Math.floor(index * regularCount / Math.max(1, takeRegular)));
    positions[index * 3] = snapshot.positions[source * 3];
    positions[index * 3 + 1] = snapshot.positions[source * 3 + 1];
    positions[index * 3 + 2] = snapshot.positions[source * 3 + 2];
    confidence[index] = snapshot.confidence[source];
  }
  for (let manual = 0; manual < manualPointCount; manual += 1) {
    const source = regularCount + manual;
    const target = takeRegular + manual;
    positions[target * 3] = snapshot.positions[source * 3];
    positions[target * 3 + 1] = snapshot.positions[source * 3 + 1];
    positions[target * 3 + 2] = snapshot.positions[source * 3 + 2];
    confidence[target] = snapshot.confidence[source];
  }
  return { positions, confidence, pointCount: outputCount, manualPointCount };
}

/** Lightweight point renderer used only during immersive AR. */
export class LiveCoveragePointRenderer {
  private readonly program: WebGLProgram;
  private readonly positionBuffer: WebGLBuffer;
  private readonly confidenceBuffer: WebGLBuffer;
  private readonly positionLocation: number;
  private readonly confidenceLocation: number;
  private readonly viewProjectionLocation: WebGLUniformLocation;
  private readonly viewMatrix = new Float32Array(16);
  private readonly viewProjection = new Float32Array(16);
  private pointCount = 0;
  private manualPointCount = 0;
  private lastDrawMs = Number.NEGATIVE_INFINITY;

  constructor(private readonly gl: GlContext) {
    this.program = createProgram(gl);
    const positionBuffer = gl.createBuffer();
    const confidenceBuffer = gl.createBuffer();
    if (positionBuffer === null || confidenceBuffer === null) {
      if (positionBuffer !== null) gl.deleteBuffer(positionBuffer);
      if (confidenceBuffer !== null) gl.deleteBuffer(confidenceBuffer);
      gl.deleteProgram(this.program);
      throw new Error('Unable to allocate coverage-guide WebGL buffers.');
    }
    this.positionBuffer = positionBuffer;
    this.confidenceBuffer = confidenceBuffer;
    this.positionLocation = gl.getAttribLocation(this.program, 'a_position');
    this.confidenceLocation = gl.getAttribLocation(this.program, 'a_confidence');
    const matrixLocation = gl.getUniformLocation(this.program, 'u_view_projection');
    if (this.positionLocation < 0 || this.confidenceLocation < 0 || matrixLocation === null) {
      this.dispose();
      throw new Error('Coverage-guide shader attributes are unavailable.');
    }
    this.viewProjectionLocation = matrixLocation;
  }

  update(snapshot: CoverageRenderSnapshot): void {
    const started = performance.now();
    const policy = activeXrRenderPolicy();
    const bounded = applyCoverageRenderBudget(snapshot, policy.pointBudget);
    this.pointCount = bounded.pointCount;
    this.manualPointCount = bounded.manualPointCount ?? 0;
    const gl = this.gl;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.positionBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, bounded.positions.subarray(0, this.pointCount * 3), gl.DYNAMIC_DRAW);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.confidenceBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, bounded.confidence.subarray(0, this.pointCount), gl.DYNAMIC_DRAW);
    gl.bindBuffer(gl.ARRAY_BUFFER, null);
    recordActiveXrPerformanceMetric('coverageUpload', performance.now() - started);
  }

  clear(): void {
    this.pointCount = 0;
    this.manualPointCount = 0;
    this.lastDrawMs = Number.NEGATIVE_INFINITY;
  }

  render(layer: CoverageRenderLayer, views: readonly CoverageRenderView[]): void {
    if (this.pointCount <= 0) return;
    const now = performance.now();
    const policy = activeXrRenderPolicy();
    if (policy.drawIntervalMs > 0 && now - this.lastDrawMs < policy.drawIntervalMs) return;
    this.lastDrawMs = now;
    const started = now;
    const gl = this.gl;
    const regularCount = this.pointCount - this.manualPointCount;
    gl.useProgram(this.program);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LEQUAL);
    gl.depthMask(false);

    gl.bindBuffer(gl.ARRAY_BUFFER, this.positionBuffer);
    gl.enableVertexAttribArray(this.positionLocation);
    gl.vertexAttribPointer(this.positionLocation, 3, gl.FLOAT, false, 0, 0);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.confidenceBuffer);
    gl.enableVertexAttribArray(this.confidenceLocation);
    gl.vertexAttribPointer(this.confidenceLocation, 1, gl.FLOAT, false, 0, 0);

    for (const view of views) {
      const viewport = layer.getViewport(view);
      gl.viewport(viewport.x, viewport.y, viewport.width, viewport.height);
      if (!rigidInverse(view.transform.matrix, this.viewMatrix)) continue;
      if (!multiplyMat4(view.projectionMatrix, this.viewMatrix, this.viewProjection)) continue;
      gl.uniformMatrix4fv(this.viewProjectionLocation, false, this.viewProjection);

      if (regularCount > 0) {
        gl.enable(gl.DEPTH_TEST);
        gl.drawArrays(gl.POINTS, 0, regularCount);
      }
      if (this.manualPointCount > 0) {
        gl.disable(gl.DEPTH_TEST);
        gl.drawArrays(gl.POINTS, regularCount, this.manualPointCount);
      }
    }

    gl.depthMask(true);
    gl.enable(gl.DEPTH_TEST);
    gl.disableVertexAttribArray(this.positionLocation);
    gl.disableVertexAttribArray(this.confidenceLocation);
    gl.bindBuffer(gl.ARRAY_BUFFER, null);
    gl.useProgram(null);
    recordActiveXrPerformanceMetric('coverageDraw', performance.now() - started);
  }

  dispose(): void {
    this.gl.deleteBuffer(this.positionBuffer);
    this.gl.deleteBuffer(this.confidenceBuffer);
    this.gl.deleteProgram(this.program);
    this.pointCount = 0;
    this.manualPointCount = 0;
    this.lastDrawMs = Number.NEGATIVE_INFINITY;
  }
}
