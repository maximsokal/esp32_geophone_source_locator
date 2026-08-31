import { useEffect, useMemo, useRef } from 'react';

import { buildSurfaceMeshPreview } from '../xr/meshPreview';
import type { ScanPointCloudSnapshot } from '../xr/webxrRuntime';
import type { ViewCommand } from './SceneViewport';

export interface ScanDisplayOptions {
  goodPoints: boolean;
  badPoints: boolean;
  dimensions: boolean;
  mesh: boolean;
  freeAir: boolean;
  unknownSpace: boolean;
}

interface ScanPointCloudViewportProps {
  snapshot: ScanPointCloudSnapshot;
  viewCommand: ViewCommand;
  display: ScanDisplayOptions;
}

interface PreparedScene {
  goodPositions: Float32Array;
  goodConfidence: Float32Array;
  badPositions: Float32Array;
  badConfidence: Float32Array;
  meshPositions: Float32Array;
  freeAirPositions: Float32Array;
  unknownPositions: Float32Array;
  boundsLines: Float32Array;
  extent: number;
  extentX: number;
  extentY: number;
  extentZ: number;
  meshTriangles: number;
}

interface OrbitState {
  yaw: number;
  pitch: number;
  distance: number;
}

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function transformProjectPositions(
  source: Float32Array,
  centerX: number,
  centerY: number,
  centerZ: number,
): Float32Array {
  const output = new Float32Array(source.length);
  for (let index = 0; index < source.length; index += 3) {
    const projectX = source[index] - centerX;
    const projectY = source[index + 1] - centerY;
    const projectZ = source[index + 2] - centerZ;
    output[index] = projectX;
    output[index + 1] = projectZ;
    output[index + 2] = -projectY;
  }
  return output;
}

function boundsLinePositions(
  snapshot: ScanPointCloudSnapshot,
  centerX: number,
  centerY: number,
  centerZ: number,
): Float32Array {
  const bounds = snapshot.bounds;
  if (bounds === null) return new Float32Array(0);
  const corners: Array<readonly [number, number, number]> = [];
  for (const x of [bounds.min[0], bounds.max[0]]) {
    for (const y of [bounds.min[1], bounds.max[1]]) {
      for (const z of [bounds.min[2], bounds.max[2]]) corners.push([x, y, z]);
    }
  }
  const edges: readonly [number, number][] = [
    [0, 1], [0, 2], [0, 4], [1, 3], [1, 5], [2, 3],
    [2, 6], [3, 7], [4, 5], [4, 6], [5, 7], [6, 7],
  ];
  const raw = new Float32Array(edges.length * 2 * 3);
  let write = 0;
  for (const [a, b] of edges) {
    for (const index of [a, b]) {
      const point = corners[index];
      raw[write++] = point[0];
      raw[write++] = point[1];
      raw[write++] = point[2];
    }
  }
  return transformProjectPositions(raw, centerX, centerY, centerZ);
}

function prepareScene(snapshot: ScanPointCloudSnapshot): PreparedScene {
  const bounds = snapshot.bounds;
  const centerX = bounds === null ? 0 : (bounds.min[0] + bounds.max[0]) * 0.5;
  const centerY = bounds === null ? 0 : (bounds.min[1] + bounds.max[1]) * 0.5;
  const centerZ = bounds === null ? 0 : (bounds.min[2] + bounds.max[2]) * 0.5;
  const extentX = bounds === null ? 0 : bounds.max[0] - bounds.min[0];
  const extentY = bounds === null ? 0 : bounds.max[1] - bounds.min[1];
  const extentZ = bounds === null ? 0 : bounds.max[2] - bounds.min[2];
  const mesh = buildSurfaceMeshPreview({
    positions: snapshot.positions,
    normals: snapshot.normals,
    confidence: snapshot.confidence,
    voxelSizeMeters: snapshot.voxelSizeMeters,
  });

  return {
    goodPositions: transformProjectPositions(snapshot.positions, centerX, centerY, centerZ),
    goodConfidence: snapshot.confidence,
    badPositions: transformProjectPositions(snapshot.tentativePositions, centerX, centerY, centerZ),
    badConfidence: snapshot.tentativeConfidence,
    meshPositions: transformProjectPositions(mesh.trianglePositions, centerX, centerY, centerZ),
    freeAirPositions: transformProjectPositions(snapshot.freeAirPositions, centerX, centerY, centerZ),
    unknownPositions: transformProjectPositions(snapshot.unknownCandidatePositions, centerX, centerY, centerZ),
    boundsLines: boundsLinePositions(snapshot, centerX, centerY, centerZ),
    extent: Math.max(0.8, extentX, extentY, extentZ),
    extentX,
    extentY,
    extentZ,
    meshTriangles: mesh.triangleCount,
  };
}

function multiplyMat4(a: Float32Array, b: Float32Array): Float32Array {
  const out = new Float32Array(16);
  for (let column = 0; column < 4; column += 1) {
    for (let row = 0; row < 4; row += 1) {
      let value = 0;
      for (let inner = 0; inner < 4; inner += 1) {
        value += a[inner * 4 + row] * b[column * 4 + inner];
      }
      out[column * 4 + row] = value;
    }
  }
  return out;
}

function perspectiveMatrix(aspect: number, near: number, far: number): Float32Array {
  const f = 1 / Math.tan((50 * Math.PI / 180) * 0.5);
  const out = new Float32Array(16);
  out[0] = f / aspect;
  out[5] = f;
  out[10] = (far + near) / (near - far);
  out[11] = -1;
  out[14] = (2 * far * near) / (near - far);
  return out;
}

function normalize3(x: number, y: number, z: number): [number, number, number] {
  const length = Math.hypot(x, y, z) || 1;
  return [x / length, y / length, z / length];
}

function lookAtMatrix(eyeX: number, eyeY: number, eyeZ: number): Float32Array {
  const [zX, zY, zZ] = normalize3(eyeX, eyeY, eyeZ);
  const [xX, xY, xZ] = normalize3(zZ, 0, -zX);
  const yX = zY * xZ - zZ * xY;
  const yY = zZ * xX - zX * xZ;
  const yZ = zX * xY - zY * xX;
  const out = new Float32Array(16);
  out[0] = xX; out[1] = yX; out[2] = zX;
  out[4] = xY; out[5] = yY; out[6] = zY;
  out[8] = xZ; out[9] = yZ; out[10] = zZ;
  out[12] = -(xX * eyeX + xY * eyeY + xZ * eyeZ);
  out[13] = -(yX * eyeX + yY * eyeY + yZ * eyeZ);
  out[14] = -(zX * eyeX + zY * eyeY + zZ * eyeZ);
  out[15] = 1;
  return out;
}

function createShader(gl: WebGLRenderingContext, type: number, source: string): WebGLShader {
  const shader = gl.createShader(type);
  if (shader === null) throw new Error('WebGL shader allocation failed.');
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const info = gl.getShaderInfoLog(shader) ?? 'Unknown shader compile error';
    gl.deleteShader(shader);
    throw new Error(info);
  }
  return shader;
}

function linkProgram(
  gl: WebGLRenderingContext,
  vertexSource: string,
  fragmentSource: string,
): WebGLProgram {
  const vertex = createShader(gl, gl.VERTEX_SHADER, vertexSource);
  const fragment = createShader(gl, gl.FRAGMENT_SHADER, fragmentSource);
  const program = gl.createProgram();
  if (program === null) {
    gl.deleteShader(vertex);
    gl.deleteShader(fragment);
    throw new Error('WebGL program allocation failed.');
  }
  gl.attachShader(program, vertex);
  gl.attachShader(program, fragment);
  gl.linkProgram(program);
  gl.deleteShader(vertex);
  gl.deleteShader(fragment);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    const info = gl.getProgramInfoLog(program) ?? 'Unknown WebGL link error';
    gl.deleteProgram(program);
    throw new Error(info);
  }
  return program;
}

function createConfidencePointProgram(gl: WebGLRenderingContext): WebGLProgram {
  return linkProgram(gl, `
    attribute vec3 aPosition;
    attribute float aConfidence;
    uniform mat4 uViewProjection;
    uniform float uPointScale;
    varying float vConfidence;
    void main() {
      gl_Position = uViewProjection * vec4(aPosition, 1.0);
      gl_PointSize = mix(2.2, 5.4, clamp(aConfidence, 0.0, 1.0)) * uPointScale;
      vConfidence = aConfidence;
    }
  `, `
    precision mediump float;
    varying float vConfidence;
    void main() {
      vec2 p = gl_PointCoord * 2.0 - 1.0;
      if (dot(p, p) > 1.0) discard;
      float t = clamp(vConfidence, 0.0, 1.0);
      vec3 low = vec3(1.00, 0.20, 0.10);
      vec3 mid = vec3(1.00, 0.78, 0.08);
      vec3 high = vec3(0.12, 1.00, 0.55);
      vec3 color = t < 0.5 ? mix(low, mid, t * 2.0) : mix(mid, high, (t - 0.5) * 2.0);
      float alpha = mix(0.18, 0.96, t);
      gl_FragColor = vec4(color, alpha);
    }
  `);
}

function createVolumePointProgram(gl: WebGLRenderingContext): WebGLProgram {
  return linkProgram(gl, `
    attribute vec3 aPosition;
    uniform mat4 uViewProjection;
    uniform float uPointSize;
    void main() {
      gl_Position = uViewProjection * vec4(aPosition, 1.0);
      gl_PointSize = uPointSize;
    }
  `, `
    precision mediump float;
    uniform vec4 uColor;
    void main() {
      vec2 p = gl_PointCoord * 2.0 - 1.0;
      if (dot(p, p) > 1.0) discard;
      gl_FragColor = uColor;
    }
  `);
}

function createSolidProgram(gl: WebGLRenderingContext): WebGLProgram {
  return linkProgram(gl, `
    attribute vec3 aPosition;
    uniform mat4 uViewProjection;
    void main() { gl_Position = uViewProjection * vec4(aPosition, 1.0); }
  `, `
    precision mediump float;
    uniform vec4 uColor;
    void main() { gl_FragColor = uColor; }
  `);
}

function applyPreset(orbit: OrbitState, command: ViewCommand, extent: number): void {
  orbit.distance = Math.max(1.5, extent * 1.7);
  if (command.preset === 'top') {
    orbit.yaw = 0;
    orbit.pitch = Math.PI * 0.5 - 0.035;
  } else if (command.preset === 'front') {
    orbit.yaw = 0;
    orbit.pitch = 0.12;
  } else {
    orbit.yaw = 0.72;
    orbit.pitch = 0.48;
  }
}

export function ScanPointCloudViewport({ snapshot, viewCommand, display }: ScanPointCloudViewportProps) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const prepared = useMemo(() => prepareScene(snapshot), [snapshot]);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (canvas === null) return;
    const gl = canvas.getContext('webgl', {
      alpha: false,
      antialias: true,
      depth: true,
      preserveDrawingBuffer: false,
    });
    if (gl === null) return;

    const confidenceProgram = createConfidencePointProgram(gl);
    const volumeProgram = createVolumePointProgram(gl);
    const solidProgram = createSolidProgram(gl);

    const confidencePosition = gl.getAttribLocation(confidenceProgram, 'aPosition');
    const confidenceValue = gl.getAttribLocation(confidenceProgram, 'aConfidence');
    const confidenceMatrix = gl.getUniformLocation(confidenceProgram, 'uViewProjection');
    const confidenceScale = gl.getUniformLocation(confidenceProgram, 'uPointScale');
    const volumePosition = gl.getAttribLocation(volumeProgram, 'aPosition');
    const volumeMatrix = gl.getUniformLocation(volumeProgram, 'uViewProjection');
    const volumePointSize = gl.getUniformLocation(volumeProgram, 'uPointSize');
    const volumeColor = gl.getUniformLocation(volumeProgram, 'uColor');
    const solidPosition = gl.getAttribLocation(solidProgram, 'aPosition');
    const solidMatrix = gl.getUniformLocation(solidProgram, 'uViewProjection');
    const solidColor = gl.getUniformLocation(solidProgram, 'uColor');

    const goodBuffer = gl.createBuffer();
    const goodConfidenceBuffer = gl.createBuffer();
    const badBuffer = gl.createBuffer();
    const badConfidenceBuffer = gl.createBuffer();
    const meshBuffer = gl.createBuffer();
    const freeAirBuffer = gl.createBuffer();
    const unknownBuffer = gl.createBuffer();
    const boundsBuffer = gl.createBuffer();

    if ([
      confidenceMatrix, confidenceScale, volumeMatrix, volumePointSize, volumeColor,
      solidMatrix, solidColor, goodBuffer, goodConfidenceBuffer, badBuffer,
      badConfidenceBuffer, meshBuffer, freeAirBuffer, unknownBuffer, boundsBuffer,
    ].some((value) => value === null)) {
      gl.deleteProgram(confidenceProgram);
      gl.deleteProgram(volumeProgram);
      gl.deleteProgram(solidProgram);
      return;
    }

    const upload = (buffer: WebGLBuffer, data: Float32Array): void => {
      gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
      gl.bufferData(gl.ARRAY_BUFFER, data, gl.STATIC_DRAW);
    };
    upload(goodBuffer!, prepared.goodPositions);
    upload(goodConfidenceBuffer!, prepared.goodConfidence);
    upload(badBuffer!, prepared.badPositions);
    upload(badConfidenceBuffer!, prepared.badConfidence);
    upload(meshBuffer!, prepared.meshPositions);
    upload(freeAirBuffer!, prepared.freeAirPositions);
    upload(unknownBuffer!, prepared.unknownPositions);
    upload(boundsBuffer!, prepared.boundsLines);

    const orbit: OrbitState = {
      yaw: 0.72,
      pitch: 0.48,
      distance: Math.max(1.5, prepared.extent * 1.7),
    };
    applyPreset(orbit, viewCommand, prepared.extent);

    let pointerId: number | null = null;
    let previousX = 0;
    let previousY = 0;
    let animationFrame = 0;

    const drawConfidencePoints = (
      positionBuffer: WebGLBuffer,
      confidenceBuffer: WebGLBuffer,
      count: number,
      matrix: Float32Array,
      dpr: number,
    ): void => {
      if (count <= 0) return;
      gl.useProgram(confidenceProgram);
      gl.bindBuffer(gl.ARRAY_BUFFER, positionBuffer);
      gl.enableVertexAttribArray(confidencePosition);
      gl.vertexAttribPointer(confidencePosition, 3, gl.FLOAT, false, 0, 0);
      gl.bindBuffer(gl.ARRAY_BUFFER, confidenceBuffer);
      gl.enableVertexAttribArray(confidenceValue);
      gl.vertexAttribPointer(confidenceValue, 1, gl.FLOAT, false, 0, 0);
      gl.uniformMatrix4fv(confidenceMatrix, false, matrix);
      gl.uniform1f(confidenceScale, Math.min(dpr, 1.5));
      gl.drawArrays(gl.POINTS, 0, count);
      gl.disableVertexAttribArray(confidencePosition);
      gl.disableVertexAttribArray(confidenceValue);
    };

    const drawVolumePoints = (
      buffer: WebGLBuffer,
      count: number,
      matrix: Float32Array,
      pointSize: number,
      color: readonly [number, number, number, number],
    ): void => {
      if (count <= 0) return;
      gl.useProgram(volumeProgram);
      gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
      gl.enableVertexAttribArray(volumePosition);
      gl.vertexAttribPointer(volumePosition, 3, gl.FLOAT, false, 0, 0);
      gl.uniformMatrix4fv(volumeMatrix, false, matrix);
      gl.uniform1f(volumePointSize, pointSize);
      gl.uniform4f(volumeColor, ...color);
      gl.drawArrays(gl.POINTS, 0, count);
      gl.disableVertexAttribArray(volumePosition);
    };

    const drawSolid = (
      buffer: WebGLBuffer,
      primitive: number,
      count: number,
      matrix: Float32Array,
      color: readonly [number, number, number, number],
    ): void => {
      if (count <= 0) return;
      gl.useProgram(solidProgram);
      gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
      gl.enableVertexAttribArray(solidPosition);
      gl.vertexAttribPointer(solidPosition, 3, gl.FLOAT, false, 0, 0);
      gl.uniformMatrix4fv(solidMatrix, false, matrix);
      gl.uniform4f(solidColor, ...color);
      gl.drawArrays(primitive, 0, count);
      gl.disableVertexAttribArray(solidPosition);
    };

    const draw = (): void => {
      const rect = canvas.getBoundingClientRect();
      const dpr = Math.min(window.devicePixelRatio || 1, 1.5);
      const width = Math.max(1, Math.round(rect.width * dpr));
      const height = Math.max(1, Math.round(rect.height * dpr));
      if (canvas.width !== width || canvas.height !== height) {
        canvas.width = width;
        canvas.height = height;
      }

      gl.viewport(0, 0, width, height);
      gl.enable(gl.DEPTH_TEST);
      gl.enable(gl.BLEND);
      gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
      gl.clearColor(0.027, 0.063, 0.098, 1);
      gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

      const horizontal = Math.cos(orbit.pitch) * orbit.distance;
      const eyeX = Math.sin(orbit.yaw) * horizontal;
      const eyeY = Math.sin(orbit.pitch) * orbit.distance;
      const eyeZ = Math.cos(orbit.yaw) * horizontal;
      const projection = perspectiveMatrix(
        width / height,
        Math.max(0.01, prepared.extent * 0.003),
        Math.max(20, prepared.extent * 12),
      );
      const matrix = multiplyMat4(projection, lookAtMatrix(eyeX, eyeY, eyeZ));

      // Unknown is not "solid rubble". It is deliberately translucent candidate
      // volume: the camera has not proved it free and the seismic source may be there.
      if (display.unknownSpace) {
        gl.depthMask(false);
        drawVolumePoints(
          unknownBuffer!,
          prepared.unknownPositions.length / 3,
          matrix,
          Math.max(2.0, 2.6 * dpr),
          [0.98, 0.67, 0.16, 0.13],
        );
        gl.depthMask(true);
      }

      // Ray-verified free space is a different semantic class from low-confidence
      // surface points, so it is intentionally blue rather than reusing confidence colours.
      if (display.freeAir) {
        gl.depthMask(false);
        drawVolumePoints(
          freeAirBuffer!,
          prepared.freeAirPositions.length / 3,
          matrix,
          Math.max(2.0, 2.8 * dpr),
          [0.18, 0.68, 1.00, 0.30],
        );
        gl.depthMask(true);
      }

      if (display.mesh) {
        gl.depthMask(false);
        drawSolid(
          meshBuffer!,
          gl.TRIANGLES,
          prepared.meshPositions.length / 3,
          matrix,
          [0.28, 0.78, 0.82, 0.27],
        );
        gl.depthMask(true);
      }
      if (display.badPoints) {
        drawConfidencePoints(
          badBuffer!,
          badConfidenceBuffer!,
          snapshot.tentativePointCount,
          matrix,
          dpr,
        );
      }
      if (display.goodPoints) {
        drawConfidencePoints(
          goodBuffer!,
          goodConfidenceBuffer!,
          snapshot.pointCount,
          matrix,
          dpr,
        );
      }
      if (display.dimensions) {
        gl.lineWidth(1);
        drawSolid(
          boundsBuffer!,
          gl.LINES,
          prepared.boundsLines.length / 3,
          matrix,
          [0.82, 0.90, 1.0, 0.90],
        );
      }
    };

    const requestDraw = (): void => {
      cancelAnimationFrame(animationFrame);
      animationFrame = requestAnimationFrame(draw);
    };

    const handlePointerDown = (event: PointerEvent): void => {
      pointerId = event.pointerId;
      previousX = event.clientX;
      previousY = event.clientY;
      canvas.setPointerCapture(event.pointerId);
    };
    const handlePointerMove = (event: PointerEvent): void => {
      if (pointerId !== event.pointerId) return;
      const deltaX = event.clientX - previousX;
      const deltaY = event.clientY - previousY;
      previousX = event.clientX;
      previousY = event.clientY;
      orbit.yaw -= deltaX * 0.008;
      orbit.pitch = clamp(orbit.pitch + deltaY * 0.006, -1.45, 1.45);
      requestDraw();
    };
    const handlePointerUp = (event: PointerEvent): void => {
      if (pointerId === event.pointerId) {
        pointerId = null;
        canvas.releasePointerCapture(event.pointerId);
      }
    };
    const handleWheel = (event: WheelEvent): void => {
      event.preventDefault();
      orbit.distance = clamp(
        orbit.distance * Math.exp(event.deltaY * 0.0012),
        Math.max(0.25, prepared.extent * 0.12),
        Math.max(16, prepared.extent * 8),
      );
      requestDraw();
    };

    const resizeObserver = new ResizeObserver(requestDraw);
    resizeObserver.observe(canvas);
    canvas.addEventListener('pointerdown', handlePointerDown);
    canvas.addEventListener('pointermove', handlePointerMove);
    canvas.addEventListener('pointerup', handlePointerUp);
    canvas.addEventListener('pointercancel', handlePointerUp);
    canvas.addEventListener('wheel', handleWheel, { passive: false });
    requestDraw();

    return () => {
      cancelAnimationFrame(animationFrame);
      resizeObserver.disconnect();
      canvas.removeEventListener('pointerdown', handlePointerDown);
      canvas.removeEventListener('pointermove', handlePointerMove);
      canvas.removeEventListener('pointerup', handlePointerUp);
      canvas.removeEventListener('pointercancel', handlePointerUp);
      canvas.removeEventListener('wheel', handleWheel);
      for (const buffer of [
        goodBuffer, goodConfidenceBuffer, badBuffer, badConfidenceBuffer,
        meshBuffer, freeAirBuffer, unknownBuffer, boundsBuffer,
      ]) {
        if (buffer !== null) gl.deleteBuffer(buffer);
      }
      gl.deleteProgram(confidenceProgram);
      gl.deleteProgram(volumeProgram);
      gl.deleteProgram(solidProgram);
    };
  }, [prepared, snapshot.pointCount, snapshot.tentativePointCount, viewCommand, display]);

  return (
    <div className="viewport-shell scan-viewport" aria-label="Measured WebXR rescue search volume">
      <canvas ref={canvasRef} className="scan-webgl-canvas" />
      <div className="scan-source-badge">
        SURFACE {snapshot.pointCount.toLocaleString()} · FREE {snapshot.knownFreeVoxelCount.toLocaleString()} · UNKNOWN {snapshot.unknownCandidateVoxelCount.toLocaleString()}
      </div>
      {display.dimensions && (
        <div className="scan-dimension-overlay">
          X {prepared.extentX.toFixed(2)} m · Y {prepared.extentY.toFixed(2)} m · Z {prepared.extentZ.toFixed(2)} m
        </div>
      )}
      {display.mesh && (
        <div className="scan-mesh-overlay">
          SURFACE BOUNDARY · {prepared.meshTriangles.toLocaleString()} triangles
        </div>
      )}
      <div className="axis-note">X / Y = local horizontal · Z = height · drag to orbit</div>
    </div>
  );
}
