import { LiveMeshRenderer, LIVE_MESH_STYLE, REFERENCE_MESH_STYLE, type LiveMeshBlendMode } from '../src/xr/liveMeshRenderer';

/*
 * Real framebuffer test bench for the live mesh renderer (native WebGL2 in Chromium, SwiftShader): draws a known camera-like background, then the REAL
 * LiveMeshRenderer, and reads the pixels back. Orthographic view: x, y in [-1, 1], depth z in [0, 1] (smaller is nearer). Triangles are given as flat
 * arrays of 9 numbers each. Only used by tests/e2e/live-mesh-pixels.spec.ts; never part of the app bundle.
 */
interface Rect { x: number; y: number; w: number; h: number; color: [number, number, number] }
interface MeshSpec { triangles: number[]; style?: 'live' | 'reference'; mode?: LiveMeshBlendMode; depthBias?: boolean }
interface Spec {
  size?: number;
  background: Rect[];
  /** Depth-only occluders drawn first, as a nearer real-world surface would be. */
  occluders?: number[][];
  meshes: MeshSpec[];
  probes: Array<[number, number]>;
  /** Replace the default GL state with this before drawing, and report what the state is afterwards. */
  preState?: boolean;
}

const canvas = document.getElementById('gl') as HTMLCanvasElement;
const gl = canvas.getContext('webgl2', { antialias: false, alpha: true, depth: true, preserveDrawingBuffer: true }) as WebGL2RenderingContext;
const identity = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
/* clip = (x, y, 2z - 1, 1) */
const projection = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2, 0, 0, 0, -1, 1]);

function snapshotState(): Record<string, unknown> {
  return {
    blend: gl.isEnabled(gl.BLEND), depthTest: gl.isEnabled(gl.DEPTH_TEST), cull: gl.isEnabled(gl.CULL_FACE), polygonOffset: gl.isEnabled(gl.POLYGON_OFFSET_FILL),
    depthMask: gl.getParameter(gl.DEPTH_WRITEMASK), depthFunc: gl.getParameter(gl.DEPTH_FUNC), colorMask: Array.from(gl.getParameter(gl.COLOR_WRITEMASK) as boolean[]),
    blendSrc: gl.getParameter(gl.BLEND_SRC_RGB), blendDst: gl.getParameter(gl.BLEND_DST_RGB), program: gl.getParameter(gl.CURRENT_PROGRAM) === null ? 'none' : 'some',
    arrayBuffer: gl.getParameter(gl.ARRAY_BUFFER_BINDING) === null ? 'none' : 'some', viewport: Array.from(gl.getParameter(gl.VIEWPORT) as Int32Array),
    polygonOffsetFactor: gl.getParameter(gl.POLYGON_OFFSET_FACTOR), polygonOffsetUnits: gl.getParameter(gl.POLYGON_OFFSET_UNITS),
  };
}

function drawDepthOnly(triangles: number[]): void {
  const vertexShader = gl.createShader(gl.VERTEX_SHADER)!;
  gl.shaderSource(vertexShader, '#version 300 es\nin vec3 p; void main(){ gl_Position = vec4(p.xy, 2.0*p.z-1.0, 1.0); }'); gl.compileShader(vertexShader);
  const fragmentShader = gl.createShader(gl.FRAGMENT_SHADER)!;
  gl.shaderSource(fragmentShader, '#version 300 es\nprecision mediump float; out vec4 c; void main(){ c = vec4(1.0); }'); gl.compileShader(fragmentShader);
  const program = gl.createProgram()!; gl.attachShader(program, vertexShader); gl.attachShader(program, fragmentShader); gl.linkProgram(program);
  const buffer = gl.createBuffer()!; gl.bindBuffer(gl.ARRAY_BUFFER, buffer); gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(triangles), gl.STATIC_DRAW);
  const location = gl.getAttribLocation(program, 'p');
  gl.useProgram(program); gl.enableVertexAttribArray(location); gl.vertexAttribPointer(location, 3, gl.FLOAT, false, 0, 0);
  gl.enable(gl.DEPTH_TEST); gl.depthFunc(gl.LESS); gl.depthMask(true); gl.colorMask(false, false, false, false);
  gl.drawArrays(gl.TRIANGLES, 0, triangles.length / 3);
  gl.colorMask(true, true, true, true); gl.disableVertexAttribArray(location); gl.bindBuffer(gl.ARRAY_BUFFER, null); gl.useProgram(null);
  gl.deleteBuffer(buffer); gl.deleteProgram(program); gl.deleteShader(vertexShader); gl.deleteShader(fragmentShader);
}

function run(spec: Spec): { pixels: number[][]; before: Record<string, unknown> | null; after: Record<string, unknown> | null; contexts: number } {
  const size = spec.size ?? 64;
  canvas.width = size; canvas.height = size;
  gl.viewport(0, 0, size, size);
  gl.disable(gl.SCISSOR_TEST); gl.colorMask(true, true, true, true); gl.depthMask(true);
  gl.clearColor(0, 0, 0, 1); gl.clearDepth(1); gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
  gl.enable(gl.SCISSOR_TEST);
  for (const rect of spec.background) {
    gl.scissor(rect.x, rect.y, rect.w, rect.h);
    gl.clearColor(rect.color[0] / 255, rect.color[1] / 255, rect.color[2] / 255, 1); gl.clear(gl.COLOR_BUFFER_BIT);
  }
  gl.disable(gl.SCISSOR_TEST);
  for (const occluder of spec.occluders ?? []) drawDepthOnly(occluder);
  let before: Record<string, unknown> | null = null;
  if (spec.preState === true) {
    gl.enable(gl.CULL_FACE); gl.depthFunc(gl.GREATER); gl.depthMask(true); gl.colorMask(true, false, true, true); gl.blendFunc(gl.ONE, gl.ZERO);
    gl.enable(gl.POLYGON_OFFSET_FILL); gl.polygonOffset(3, 4); gl.disable(gl.BLEND); gl.disable(gl.DEPTH_TEST);
    before = snapshotState();
  }
  const layer = { framebuffer: null, getViewport: () => ({ x: 0, y: 0, width: size, height: size }) };
  const views = [{ projectionMatrix: projection, transform: { matrix: identity } }];
  for (const mesh of spec.meshes) {
    const renderer = new LiveMeshRenderer(gl, { style: mesh.style === 'reference' ? REFERENCE_MESH_STYLE : LIVE_MESH_STYLE, ...(mesh.mode === undefined ? {} : { mode: mesh.mode }), depthBias: mesh.depthBias ?? false });  /* no mode given: the renderer's own default is what is tested */
    renderer.update(Float32Array.from(mesh.triangles), mesh.triangles.length / 9);
    renderer.render(layer, views);
    renderer.dispose();
  }
  const after = spec.preState === true ? snapshotState() : null;
  const pixel = new Uint8Array(4);
  const pixels = spec.probes.map(([x, y]) => { gl.readPixels(x, y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, pixel); return [pixel[0], pixel[1], pixel[2], pixel[3]]; });
  return { pixels, before, after, contexts: document.querySelectorAll('canvas').length };
}

/** Wall-clock cost of drawing one mesh, per frame, with the GPU (here: SwiftShader, software) finished before the clock stops. Relative numbers only. */
function bench(spec: { triangles: number[]; mode: LiveMeshBlendMode; frames: number; size?: number }): { msPerFrame: number; p95: number } {
  const size = spec.size ?? 256;
  canvas.width = size; canvas.height = size;
  gl.viewport(0, 0, size, size);
  const layer = { framebuffer: null, getViewport: () => ({ x: 0, y: 0, width: size, height: size }) };
  const views = [{ projectionMatrix: projection, transform: { matrix: identity } }];
  const renderer = new LiveMeshRenderer(gl, { mode: spec.mode });
  renderer.update(Float32Array.from(spec.triangles), spec.triangles.length / 9);
  const samples: number[] = [];
  for (let frame = 0; frame < spec.frames + 3; frame += 1) {
    gl.clearColor(0, 0, 0, 1); gl.clearDepth(1); gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    const t0 = performance.now();
    renderer.render(layer, views);
    gl.finish();
    if (frame >= 3) samples.push(performance.now() - t0);
  }
  renderer.dispose();
  samples.sort((a, b) => a - b);
  return { msPerFrame: samples.reduce((x, y) => x + y, 0) / samples.length, p95: samples[Math.min(samples.length - 1, Math.ceil(0.95 * samples.length) - 1)] };
}

(window as unknown as { __liveMeshPixels: { run: typeof run; bench: typeof bench; ready: boolean } }).__liveMeshPixels = { run, bench, ready: gl !== null };
