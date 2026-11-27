import { LiveCoveragePointRenderer } from '../src/xr/liveCoverageRenderer';
import { LiveMeshRenderer, LIVE_MESH_STYLE, REFERENCE_MESH_STYLE } from '../src/xr/liveMeshRenderer';
import { XrPerformanceProfiler } from '../src/xr/xrPerformanceProfiler';

/*
 * Real framebuffer test bench for the live mesh renderer (native WebGL2 in Chromium, SwiftShader). The canvas is set up like the XR layer: transparent,
 * premultiplied alpha, cleared to (0,0,0,0), so that what is read back is exactly what the AR compositor would put over the camera. Orthographic view:
 * x, y in [-1, 1], depth z in [0, 1] (smaller is nearer). Triangles are flat arrays of 9 numbers each. Only used by tests/e2e/live-mesh-pixels.spec.ts.
 */
interface MeshSpec { triangles: number[]; style: 'live' | 'reference' }
interface Spec {
  meshes: MeshSpec[];
  probes: Array<[number, number]>;
  /** Depth-only occluders drawn first, as something that wrote real depth would be. */
  occluders?: number[][];
  /** An opaque red triangle drawn AFTER the meshes with the depth test: it shows only where the meshes left the depth buffer alone. */
  behind?: number[];
  /** Coverage-guide points drawn after the meshes with the production point renderer (x, y, z each). */
  guide?: number[];
  /** Replace the default GL state with another before drawing, and report the state before and after. */
  preState?: boolean;
  /** Run under HIGH frame pressure and draw this many consecutive frames, probing the last one. */
  pressuredFrames?: number;
}

const SIZE = 64;
const canvas = document.getElementById('gl') as HTMLCanvasElement;
const gl = canvas.getContext('webgl2', { antialias: false, alpha: true, premultipliedAlpha: true, depth: true, stencil: false, preserveDrawingBuffer: true }) as WebGL2RenderingContext;
const identity = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
/* clip = (x, y, 2z - 1, 1) */
const projection = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2, 0, 0, 0, -1, 1]);
const layer = { framebuffer: null, getViewport: () => ({ x: 0, y: 0, width: SIZE, height: SIZE }) };
const views = [{ projectionMatrix: projection, transform: { matrix: identity } }];

function snapshotState(): Record<string, unknown> {
  return {
    blend: gl.isEnabled(gl.BLEND), depthTest: gl.isEnabled(gl.DEPTH_TEST), cull: gl.isEnabled(gl.CULL_FACE), stencil: gl.isEnabled(gl.STENCIL_TEST),
    depthMask: gl.getParameter(gl.DEPTH_WRITEMASK), depthFunc: gl.getParameter(gl.DEPTH_FUNC), colorMask: Array.from(gl.getParameter(gl.COLOR_WRITEMASK) as boolean[]),
    blendSrc: gl.getParameter(gl.BLEND_SRC_RGB), blendDst: gl.getParameter(gl.BLEND_DST_RGB), blendEquation: gl.getParameter(gl.BLEND_EQUATION_RGB),
    program: gl.getParameter(gl.CURRENT_PROGRAM) === null ? 'none' : 'some', arrayBuffer: gl.getParameter(gl.ARRAY_BUFFER_BINDING) === null ? 'none' : 'some',
    viewport: Array.from(gl.getParameter(gl.VIEWPORT) as Int32Array),
  };
}

/** A plain triangle list drawn by the test itself: depth only, or opaque red under the depth test. */
function drawPlain(triangles: number[], kind: 'depth' | 'red'): void {
  const shader = (type: number, source: string): WebGLShader => { const s = gl.createShader(type)!; gl.shaderSource(s, source); gl.compileShader(s); return s; };
  const vertex = shader(gl.VERTEX_SHADER, '#version 300 es\nin vec3 p; void main(){ gl_Position = vec4(p.xy, 2.0*p.z-1.0, 1.0); }');
  const fragment = shader(gl.FRAGMENT_SHADER, '#version 300 es\nprecision mediump float; out vec4 c; void main(){ c = vec4(1.0, 0.0, 0.0, 1.0); }');
  const program = gl.createProgram()!; gl.attachShader(program, vertex); gl.attachShader(program, fragment); gl.linkProgram(program);
  const buffer = gl.createBuffer()!; gl.bindBuffer(gl.ARRAY_BUFFER, buffer); gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(triangles), gl.STATIC_DRAW);
  const location = gl.getAttribLocation(program, 'p');
  gl.useProgram(program); gl.enableVertexAttribArray(location); gl.vertexAttribPointer(location, 3, gl.FLOAT, false, 0, 0);
  gl.disable(gl.BLEND); gl.enable(gl.DEPTH_TEST); gl.depthFunc(gl.LESS); gl.depthMask(kind === 'depth');
  const colour = kind === 'red'; gl.colorMask(colour, colour, colour, colour);
  gl.drawArrays(gl.TRIANGLES, 0, triangles.length / 3);
  gl.depthMask(true); gl.colorMask(true, true, true, true); gl.disableVertexAttribArray(location); gl.bindBuffer(gl.ARRAY_BUFFER, null); gl.useProgram(null);
  gl.deleteBuffer(buffer); gl.deleteProgram(program); gl.deleteShader(vertex); gl.deleteShader(fragment);
}

function clearLikeXr(): void {
  gl.disable(gl.SCISSOR_TEST); gl.colorMask(true, true, true, true); gl.depthMask(true);
  gl.clearColor(0, 0, 0, 0); gl.clearDepth(1); gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
}

function run(spec: Spec): { pixels: number[][]; triangles: number[]; before: Record<string, unknown> | null; after: Record<string, unknown> | null; contexts: number; glError: number } {
  canvas.width = SIZE; canvas.height = SIZE;
  gl.viewport(0, 0, SIZE, SIZE);
  clearLikeXr();
  for (const occluder of spec.occluders ?? []) drawPlain(occluder, 'depth');
  let before: Record<string, unknown> | null = null;
  if (spec.preState === true) {
    gl.enable(gl.CULL_FACE); gl.depthFunc(gl.GREATER); gl.depthMask(true); gl.colorMask(true, false, true, true); gl.blendFunc(gl.ONE, gl.ZERO);
    gl.blendEquation(gl.MAX); gl.enable(gl.STENCIL_TEST); gl.disable(gl.BLEND); gl.disable(gl.DEPTH_TEST);
    before = snapshotState();
  }
  const renderers = spec.meshes.map((mesh) => {
    const renderer = new LiveMeshRenderer(gl, { style: mesh.style === 'reference' ? REFERENCE_MESH_STYLE : LIVE_MESH_STYLE });
    renderer.update(Float32Array.from(mesh.triangles), mesh.triangles.length / 9);
    return renderer;
  });
  const triangles = renderers.map((renderer) => renderer.triangleCount);
  if (spec.pressuredFrames !== undefined) {
    const profiler = new XrPerformanceProfiler(); const token = profiler.activate();
    for (let i = 0; i < 200; i += 1) profiler.recordFrameTimestamp(i * 60);                 /* 60 ms frames: high pressure */
    if (profiler.snapshot().pressure !== 'high' || profiler.renderPolicy().drawIntervalMs <= 0) throw new Error('the profiler is not under high pressure');
    for (let frame = 0; frame < spec.pressuredFrames; frame += 1) { clearLikeXr(); for (const renderer of renderers) renderer.render(layer, views); }
    profiler.deactivate(token);
  } else for (const renderer of renderers) renderer.render(layer, views);
  const after = spec.preState === true ? snapshotState() : null;
  const glError = gl.getError();
  for (const renderer of renderers) renderer.dispose();
  if (spec.preState === true) { gl.disable(gl.STENCIL_TEST); gl.blendEquation(gl.FUNC_ADD); }
  if (spec.guide !== undefined) {
    const points = new LiveCoveragePointRenderer(gl);
    points.update({ positions: Float32Array.from(spec.guide), confidence: new Float32Array(spec.guide.length / 3).fill(1), pointCount: spec.guide.length / 3 });
    /* the point renderer expects the XR camera; the orthographic test view keeps the points where they are given */
    points.render(layer, views);
    points.dispose();
  }
  if (spec.behind !== undefined) drawPlain(spec.behind, 'red');
  const pixel = new Uint8Array(4);
  const pixels = spec.probes.map(([x, y]) => { gl.readPixels(x, y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, pixel); return [pixel[0], pixel[1], pixel[2], pixel[3]]; });
  return { pixels, triangles, before, after, contexts: document.querySelectorAll('canvas').length, glError };
}

/** Wall-clock cost of drawing one mesh per frame, GPU (SwiftShader: software) finished before the clock stops. Relative numbers only. */
function bench(spec: { triangles: number[]; frames: number }): { triangles: number; msPerFrame: number; p95: number } {
  canvas.width = 256; canvas.height = 256;
  const big = { framebuffer: null, getViewport: () => ({ x: 0, y: 0, width: 256, height: 256 }) };
  const renderer = new LiveMeshRenderer(gl);
  renderer.update(Float32Array.from(spec.triangles), spec.triangles.length / 9);
  const samples: number[] = [];
  for (let frame = 0; frame < spec.frames + 3; frame += 1) {
    clearLikeXr();
    const t0 = performance.now();
    renderer.render(big, views);
    gl.finish();
    if (frame >= 3) samples.push(performance.now() - t0);
  }
  const triangles = renderer.triangleCount;
  renderer.dispose();
  samples.sort((a, b) => a - b);
  return { triangles, msPerFrame: samples.reduce((x, y) => x + y, 0) / samples.length, p95: samples[Math.min(samples.length - 1, Math.ceil(0.95 * samples.length) - 1)] };
}

(window as unknown as { __liveMeshPixels: { run: typeof run; bench: typeof bench; ready: boolean } }).__liveMeshPixels = { run, bench, ready: gl !== null };
