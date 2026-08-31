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
  /** Per-point coverage confidence in [0, 1]. */
  confidence: Float32Array;
  pointCount: number;
}

type GlContext = WebGLRenderingContext | WebGL2RenderingContext;

function isWebGl2(gl: GlContext): gl is WebGL2RenderingContext {
  return typeof WebGL2RenderingContext !== 'undefined' && gl instanceof WebGL2RenderingContext;
}

function compileShader(gl: GlContext, type: number, source: string): WebGLShader {
  const shader = gl.createShader(type);
  if (shader === null) {
    throw new Error('Unable to allocate coverage-guide shader.');
  }

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
        gl_PointSize = mix(3.0, 7.5, a_confidence);
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
        gl_PointSize = mix(3.0, 7.5, a_confidence);
        v_confidence = a_confidence;
      }
    `;

  const fragmentSource = webGl2
    ? `#version 300 es
      precision mediump float;
      in float v_confidence;
      out vec4 out_color;
      void main() {
        float radius = distance(gl_PointCoord, vec2(0.5));
        if (radius > 0.5) {
          discard;
        }

        float t = clamp(v_confidence, 0.0, 1.0);
        vec3 low = vec3(1.00, 0.22, 0.08);
        vec3 mid = vec3(1.00, 0.82, 0.08);
        vec3 high = vec3(0.12, 1.00, 0.55);
        vec3 color = t < 0.5
          ? mix(low, mid, t * 2.0)
          : mix(mid, high, (t - 0.5) * 2.0);

        float edge = 1.0 - smoothstep(0.30, 0.50, radius);
        float alpha = mix(0.10, 0.92, smoothstep(0.0, 1.0, t)) * edge;
        out_color = vec4(color, alpha);
      }
    `
    : `
      precision mediump float;
      varying float v_confidence;
      void main() {
        float radius = distance(gl_PointCoord, vec2(0.5));
        if (radius > 0.5) {
          discard;
        }

        float t = clamp(v_confidence, 0.0, 1.0);
        vec3 low = vec3(1.00, 0.22, 0.08);
        vec3 mid = vec3(1.00, 0.82, 0.08);
        vec3 high = vec3(0.12, 1.00, 0.55);
        vec3 color = t < 0.5
          ? mix(low, mid, t * 2.0)
          : mix(mid, high, (t - 0.5) * 2.0);

        float edge = 1.0 - smoothstep(0.30, 0.50, radius);
        float alpha = mix(0.10, 0.92, smoothstep(0.0, 1.0, t)) * edge;
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
  if (matrix.length < 16) {
    return false;
  }

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

  if (![m0, m1, m2, m4, m5, m6, m8, m9, m10, tx, ty, tz].every(Number.isFinite)) {
    return false;
  }

  out[0] = m0;
  out[1] = m4;
  out[2] = m8;
  out[3] = 0;
  out[4] = m1;
  out[5] = m5;
  out[6] = m9;
  out[7] = 0;
  out[8] = m2;
  out[9] = m6;
  out[10] = m10;
  out[11] = 0;
  out[12] = -(m0 * tx + m1 * ty + m2 * tz);
  out[13] = -(m4 * tx + m5 * ty + m6 * tz);
  out[14] = -(m8 * tx + m9 * ty + m10 * tz);
  out[15] = 1;
  return true;
}

function multiplyMat4(
  left: ArrayLike<number>,
  right: ArrayLike<number>,
  out: Float32Array,
): boolean {
  if (left.length < 16 || right.length < 16) {
    return false;
  }

  for (let column = 0; column < 4; column += 1) {
    const r0 = Number(right[column * 4]);
    const r1 = Number(right[column * 4 + 1]);
    const r2 = Number(right[column * 4 + 2]);
    const r3 = Number(right[column * 4 + 3]);

    out[column * 4] =
      Number(left[0]) * r0 +
      Number(left[4]) * r1 +
      Number(left[8]) * r2 +
      Number(left[12]) * r3;
    out[column * 4 + 1] =
      Number(left[1]) * r0 +
      Number(left[5]) * r1 +
      Number(left[9]) * r2 +
      Number(left[13]) * r3;
    out[column * 4 + 2] =
      Number(left[2]) * r0 +
      Number(left[6]) * r1 +
      Number(left[10]) * r2 +
      Number(left[14]) * r3;
    out[column * 4 + 3] =
      Number(left[3]) * r0 +
      Number(left[7]) * r1 +
      Number(left[11]) * r2 +
      Number(left[15]) * r3;
  }

  return true;
}

/**
 * Lightweight point renderer used only during immersive AR.
 *
 * The expensive spatial decimation runs on the CPU at depth-sample cadence.
 * GPU buffers are refreshed at a much lower cadence by WebXrRuntime and then
 * rendered in one POINTS draw call per XRView. This keeps the UX useful without
 * turning every camera frame into a point-cloud rebuild.
 */
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
    const count = Math.min(
      snapshot.pointCount,
      Math.floor(snapshot.positions.length / 3),
      snapshot.confidence.length,
    );

    this.pointCount = Math.max(0, count);
    const gl = this.gl;

    gl.bindBuffer(gl.ARRAY_BUFFER, this.positionBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      this.pointCount === snapshot.pointCount
        ? snapshot.positions
        : snapshot.positions.subarray(0, this.pointCount * 3),
      gl.DYNAMIC_DRAW,
    );

    gl.bindBuffer(gl.ARRAY_BUFFER, this.confidenceBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      this.pointCount === snapshot.pointCount
        ? snapshot.confidence
        : snapshot.confidence.subarray(0, this.pointCount),
      gl.DYNAMIC_DRAW,
    );

    gl.bindBuffer(gl.ARRAY_BUFFER, null);
  }

  clear(): void {
    this.pointCount = 0;
  }

  render(layer: CoverageRenderLayer, views: readonly CoverageRenderView[]): void {
    if (this.pointCount <= 0) {
      return;
    }

    const gl = this.gl;
    gl.useProgram(this.program);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LEQUAL);
    gl.depthMask(true);

    gl.bindBuffer(gl.ARRAY_BUFFER, this.positionBuffer);
    gl.enableVertexAttribArray(this.positionLocation);
    gl.vertexAttribPointer(this.positionLocation, 3, gl.FLOAT, false, 0, 0);

    gl.bindBuffer(gl.ARRAY_BUFFER, this.confidenceBuffer);
    gl.enableVertexAttribArray(this.confidenceLocation);
    gl.vertexAttribPointer(this.confidenceLocation, 1, gl.FLOAT, false, 0, 0);

    for (const view of views) {
      const viewport = layer.getViewport(view);
      gl.viewport(viewport.x, viewport.y, viewport.width, viewport.height);

      if (!rigidInverse(view.transform.matrix, this.viewMatrix)) {
        continue;
      }
      if (!multiplyMat4(view.projectionMatrix, this.viewMatrix, this.viewProjection)) {
        continue;
      }

      gl.uniformMatrix4fv(this.viewProjectionLocation, false, this.viewProjection);
      gl.drawArrays(gl.POINTS, 0, this.pointCount);
    }

    gl.disableVertexAttribArray(this.positionLocation);
    gl.disableVertexAttribArray(this.confidenceLocation);
    gl.bindBuffer(gl.ARRAY_BUFFER, null);
    gl.useProgram(null);
  }

  dispose(): void {
    this.gl.deleteBuffer(this.positionBuffer);
    this.gl.deleteBuffer(this.confidenceBuffer);
    this.gl.deleteProgram(this.program);
    this.pointCount = 0;
  }
}
