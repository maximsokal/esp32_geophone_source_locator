import { useEffect, useMemo, useRef } from 'react';

import type { ScanPointCloudSnapshot } from '../xr/webxrRuntime';
import type { ViewCommand } from './SceneViewport';

interface ScanPointCloudViewportProps {
  snapshot: ScanPointCloudSnapshot;
  viewCommand: ViewCommand;
}

interface PreparedCloud {
  positions: Float32Array;
  extent: number;
}

interface OrbitState {
  yaw: number;
  pitch: number;
  distance: number;
}

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function prepareCloud(snapshot: ScanPointCloudSnapshot): PreparedCloud {
  const source = snapshot.positions;
  const output = new Float32Array(source.length);
  const bounds = snapshot.bounds;

  const centerX = bounds === null ? 0 : (bounds.min[0] + bounds.max[0]) * 0.5;
  const centerY = bounds === null ? 0 : (bounds.min[1] + bounds.max[1]) * 0.5;
  const centerZ = bounds === null ? 0 : (bounds.min[2] + bounds.max[2]) * 0.5;

  for (let index = 0; index < source.length; index += 3) {
    const projectX = source[index] - centerX;
    const projectY = source[index + 1] - centerY;
    const projectZ = source[index + 2] - centerZ;

    output[index] = projectX;
    output[index + 1] = projectZ;
    output[index + 2] = -projectY;
  }

  const extent =
    bounds === null
      ? 2
      : Math.max(
          0.8,
          bounds.max[0] - bounds.min[0],
          bounds.max[1] - bounds.min[1],
          bounds.max[2] - bounds.min[2],
        );

  return { positions: output, extent };
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
  const fovRadians = (50 * Math.PI) / 180;
  const f = 1 / Math.tan(fovRadians * 0.5);
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
  out[0] = xX;
  out[1] = yX;
  out[2] = zX;
  out[4] = xY;
  out[5] = yY;
  out[6] = zY;
  out[8] = xZ;
  out[9] = yZ;
  out[10] = zZ;
  out[12] = -(xX * eyeX + xY * eyeY + xZ * eyeZ);
  out[13] = -(yX * eyeX + yY * eyeY + yZ * eyeZ);
  out[14] = -(zX * eyeX + zY * eyeY + zZ * eyeZ);
  out[15] = 1;
  return out;
}

function createShader(gl: WebGLRenderingContext, type: number, source: string): WebGLShader {
  const shader = gl.createShader(type);
  if (shader === null) {
    throw new Error('WebGL shader allocation failed.');
  }

  gl.shaderSource(shader, source);
  gl.compileShader(shader);

  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const info = gl.getShaderInfoLog(shader) ?? 'Unknown shader compile error';
    gl.deleteShader(shader);
    throw new Error(info);
  }

  return shader;
}

function createProgram(gl: WebGLRenderingContext): WebGLProgram {
  const vertex = createShader(
    gl,
    gl.VERTEX_SHADER,
    `
      attribute vec3 aPosition;
      uniform mat4 uViewProjection;
      uniform float uPointSize;
      void main() {
        gl_Position = uViewProjection * vec4(aPosition, 1.0);
        gl_PointSize = uPointSize;
      }
    `,
  );

  const fragment = createShader(
    gl,
    gl.FRAGMENT_SHADER,
    `
      precision mediump float;
      void main() {
        vec2 p = gl_PointCoord * 2.0 - 1.0;
        if (dot(p, p) > 1.0) discard;
        gl_FragColor = vec4(0.40, 0.91, 0.97, 0.94);
      }
    `,
  );

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

export function ScanPointCloudViewport({ snapshot, viewCommand }: ScanPointCloudViewportProps) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const prepared = useMemo(() => prepareCloud(snapshot), [snapshot]);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (canvas === null) {
      return;
    }

    const gl = canvas.getContext('webgl', {
      alpha: false,
      antialias: true,
      depth: true,
      preserveDrawingBuffer: false,
    });

    if (gl === null) {
      return;
    }

    const program = createProgram(gl);
    const positionLocation = gl.getAttribLocation(program, 'aPosition');
    const matrixLocation = gl.getUniformLocation(program, 'uViewProjection');
    const pointSizeLocation = gl.getUniformLocation(program, 'uPointSize');
    const buffer = gl.createBuffer();

    if (buffer === null || matrixLocation === null || pointSizeLocation === null) {
      gl.deleteProgram(program);
      return;
    }

    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(gl.ARRAY_BUFFER, prepared.positions, gl.STATIC_DRAW);

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

      const near = Math.max(0.01, prepared.extent * 0.003);
      const far = Math.max(20, prepared.extent * 12);
      const projection = perspectiveMatrix(width / height, near, far);
      const view = lookAtMatrix(eyeX, eyeY, eyeZ);
      const viewProjection = multiplyMat4(projection, view);

      gl.useProgram(program);
      gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
      gl.enableVertexAttribArray(positionLocation);
      gl.vertexAttribPointer(positionLocation, 3, gl.FLOAT, false, 0, 0);
      gl.uniformMatrix4fv(matrixLocation, false, viewProjection);
      gl.uniform1f(pointSizeLocation, clamp(2.2 * dpr, 2.2, 4.2));
      gl.drawArrays(gl.POINTS, 0, snapshot.pointCount);
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
      if (pointerId !== event.pointerId) {
        return;
      }

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
      const factor = Math.exp(event.deltaY * 0.0012);
      orbit.distance = clamp(
        orbit.distance * factor,
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
      gl.deleteBuffer(buffer);
      gl.deleteProgram(program);
    };
  }, [prepared, snapshot.pointCount, viewCommand]);

  return (
    <div className="viewport-shell scan-viewport" aria-label="Measured WebXR depth point cloud">
      <canvas ref={canvasRef} className="scan-webgl-canvas" />
      <div className="scan-source-badge">REAL WEBXR DEPTH · {snapshot.pointCount.toLocaleString()} POINTS</div>
      <div className="axis-note">X / Y = local horizontal · Z = height · drag to orbit</div>
    </div>
  );
}
