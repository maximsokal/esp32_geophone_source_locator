import { afterEach, describe, expect, it, vi } from 'vitest';

import * as rgbGuidance from '../../src/xr/rgbDepthGuidance';
import { RawCameraGuidanceCapture, type XrCameraViewLike } from '../../src/xr/rawCameraGuidance';
import { TagDetectorClient } from '../../src/xr/fiducial/tagDetectorClient';
import { LiveCoveragePointRenderer } from '../../src/xr/liveCoverageRenderer';

interface FakeAttribute {
  enabled: boolean;
  buffer: object | null;
  size: number;
  type: number;
  normalized: boolean;
  stride: number;
  offset: number;
}

function fakeGl(pixels: Uint8Array) {
  const c = {
    ACTIVE_TEXTURE: 1, TEXTURE_BINDING_2D: 2, FRAMEBUFFER_BINDING: 3, VIEWPORT: 4, CURRENT_PROGRAM: 5,
    ARRAY_BUFFER_BINDING: 6, TEXTURE0: 100, TEXTURE_2D: 101, ARRAY_BUFFER: 102, FRAMEBUFFER: 103,
    VERTEX_ATTRIB_ARRAY_ENABLED: 104, VERTEX_ATTRIB_ARRAY_BUFFER_BINDING: 105, VERTEX_ATTRIB_ARRAY_SIZE: 106,
    VERTEX_ATTRIB_ARRAY_TYPE: 107, VERTEX_ATTRIB_ARRAY_NORMALIZED: 108, VERTEX_ATTRIB_ARRAY_STRIDE: 109,
    VERTEX_ATTRIB_ARRAY_POINTER: 110, BLEND: 111, DEPTH_TEST: 112, SCISSOR_TEST: 113, CULL_FACE: 114,
    STENCIL_TEST: 115, COLOR_WRITEMASK: 116, DEPTH_WRITEMASK: 117,
    BLEND_SRC_RGB: 118, BLEND_DST_RGB: 119, BLEND_SRC_ALPHA: 120, BLEND_DST_ALPHA: 121,
    BLEND_EQUATION_RGB: 122, BLEND_EQUATION_ALPHA: 123, DEPTH_FUNC: 124,
    VERTEX_SHADER: 200, FRAGMENT_SHADER: 201, COMPILE_STATUS: 202, LINK_STATUS: 203, FLOAT: 204,
    STATIC_DRAW: 205, TRIANGLES: 206, RGBA: 207, UNSIGNED_BYTE: 208, FRAMEBUFFER_COMPLETE: 209,
    COLOR_ATTACHMENT0: 210, TEXTURE_MIN_FILTER: 211, TEXTURE_MAG_FILTER: 212, TEXTURE_WRAP_S: 213,
    TEXTURE_WRAP_T: 214, LINEAR: 215, CLAMP_TO_EDGE: 216, POINTS: 217, SRC_ALPHA: 218,
    ONE_MINUS_SRC_ALPHA: 219, FUNC_ADD: 220, LEQUAL: 221, DYNAMIC_DRAW: 222,
  };
  const enabled = new Set<number>([c.BLEND, c.DEPTH_TEST, c.SCISSOR_TEST, c.CULL_FACE, c.STENCIL_TEST]);
  let activeTexture = c.TEXTURE0;
  const textures = new Map<number, object | null>([[c.TEXTURE0, { id: 'prior-texture' }]]);
  let framebuffer: object | null = { id: 'prior-framebuffer' };
  let viewport = new Int32Array([11, 12, 640, 480]);
  let program: object | null = { id: 'prior-program' };
  let arrayBuffer: object | null = { id: 'prior-array-buffer' };
  let depthMask = false;
  let colorMask: [boolean, boolean, boolean, boolean] = [true, false, true, false];
  let blendFactors: [number, number, number, number] = [770, 771, 1, 0];
  let blendEquations: [number, number] = [32774, 32778];
  let depthFunc = 515;
  const attributes = new Map<number, FakeAttribute>([[3, {
    enabled: true, buffer: { id: 'prior-attribute-buffer' }, size: 3, type: c.FLOAT,
    normalized: true, stride: 24, offset: 8,
  }], [4, {
    enabled: false, buffer: { id: 'prior-confidence-buffer' }, size: 2, type: c.FLOAT,
    normalized: false, stride: 12, offset: 4,
  }]]);
  const created: { kind: string; value: object }[] = [];
  const deleted: string[] = [];
  const drawRasterStates: object[] = [];
  const readbackTargets: Uint8Array[] = [];
  const make = (kind: string): object => { const value = { kind, id: created.length }; created.push({ kind, value }); return value; };
  const gl: any = {
    ...c,
    getParameter(parameter: number) {
      switch (parameter) {
        case c.ACTIVE_TEXTURE: return activeTexture;
        case c.TEXTURE_BINDING_2D: return textures.get(activeTexture) ?? null;
        case c.FRAMEBUFFER_BINDING: return framebuffer;
        case c.VIEWPORT: return viewport;
        case c.CURRENT_PROGRAM: return program;
        case c.ARRAY_BUFFER_BINDING: return arrayBuffer;
        case c.DEPTH_WRITEMASK: return depthMask;
        case c.COLOR_WRITEMASK: return colorMask;
        case c.BLEND_SRC_RGB: return blendFactors[0];
        case c.BLEND_DST_RGB: return blendFactors[1];
        case c.BLEND_SRC_ALPHA: return blendFactors[2];
        case c.BLEND_DST_ALPHA: return blendFactors[3];
        case c.BLEND_EQUATION_RGB: return blendEquations[0];
        case c.BLEND_EQUATION_ALPHA: return blendEquations[1];
        case c.DEPTH_FUNC: return depthFunc;
        default: return null;
      }
    },
    isEnabled(capability: number) { return enabled.has(capability); },
    enable(capability: number) { enabled.add(capability); },
    disable(capability: number) { enabled.delete(capability); },
    colorMask(r: boolean, g: boolean, b: boolean, a: boolean) { colorMask = [r, g, b, a]; },
    depthMask(value: boolean) { depthMask = value; },
    createFramebuffer() { return make('framebuffer'); }, deleteFramebuffer() { deleted.push('framebuffer'); },
    createTexture() { return make('texture'); }, deleteTexture() { deleted.push('texture'); },
    createBuffer() { return make('buffer'); }, deleteBuffer() { deleted.push('buffer'); },
    createShader() { return make('shader'); }, shaderSource() {}, compileShader() {}, getShaderParameter() { return true; },
    getShaderInfoLog() { return ''; }, deleteShader() { deleted.push('shader'); },
    createProgram() { return make('program'); }, attachShader() {}, linkProgram() {}, getProgramParameter() { return true; },
    getProgramInfoLog() { return ''; }, deleteProgram() { deleted.push('program'); },
    getAttribLocation(_program: unknown, name: string) { return name === 'a_position' ? 3 : 4; }, getUniformLocation() { return {}; },
    activeTexture(value: number) { activeTexture = value; }, bindTexture(_target: number, value: object | null) { textures.set(activeTexture, value); },
    texParameteri() {}, texImage2D() {},
    bindFramebuffer(_target: number, value: object | null) { framebuffer = value; },
    framebufferTexture2D() {}, checkFramebufferStatus() { return c.FRAMEBUFFER_COMPLETE; },
    bindBuffer(_target: number, value: object | null) { arrayBuffer = value; }, bufferData() {},
    getVertexAttrib(location: number, parameter: number) {
      const attribute = attributes.get(location)!;
      switch (parameter) {
        case c.VERTEX_ATTRIB_ARRAY_ENABLED: return attribute.enabled;
        case c.VERTEX_ATTRIB_ARRAY_BUFFER_BINDING: return attribute.buffer;
        case c.VERTEX_ATTRIB_ARRAY_SIZE: return attribute.size;
        case c.VERTEX_ATTRIB_ARRAY_TYPE: return attribute.type;
        case c.VERTEX_ATTRIB_ARRAY_NORMALIZED: return attribute.normalized;
        case c.VERTEX_ATTRIB_ARRAY_STRIDE: return attribute.stride;
        default: return null;
      }
    },
    getVertexAttribOffset(location: number) { return attributes.get(location)!.offset; },
    enableVertexAttribArray(location: number) { attributes.get(location)!.enabled = true; },
    disableVertexAttribArray(location: number) { attributes.get(location)!.enabled = false; },
    vertexAttribPointer(location: number, size: number, type: number, normalized: boolean, stride: number, offset: number) {
      attributes.set(location, { enabled: attributes.get(location)?.enabled ?? false, buffer: arrayBuffer, size, type, normalized, stride, offset });
    },
    useProgram(value: object | null) { program = value; },
    blendFunc(src: number, dst: number) { blendFactors = [src, dst, src, dst]; },
    blendFuncSeparate(srcRgb: number, dstRgb: number, srcAlpha: number, dstAlpha: number) { blendFactors = [srcRgb, dstRgb, srcAlpha, dstAlpha]; },
    blendEquationSeparate(rgb: number, alpha: number) { blendEquations = [rgb, alpha]; },
    depthFunc(value: number) { depthFunc = value; },
    viewport(x: number, y: number, width: number, height: number) { viewport = new Int32Array([x, y, width, height]); },
    uniform1i() {}, uniformMatrix4fv() {},
    drawArrays() {
      drawRasterStates.push({ blend: enabled.has(c.BLEND), depth: enabled.has(c.DEPTH_TEST), scissor: enabled.has(c.SCISSOR_TEST),
        cull: enabled.has(c.CULL_FACE), stencil: enabled.has(c.STENCIL_TEST), colorMask: [...colorMask], depthMask });
    },
    readPixels(_x: number, _y: number, _width: number, _height: number, _format: number, _type: number, destination: Uint8Array) {
      readbackTargets.push(destination);
      destination.set(pixels.subarray(0, destination.length));
    },
  };
  return {
    gl: gl as WebGLRenderingContext,
    created,
    deleted,
    drawRasterStates,
    readbackTargets,
    state: () => ({ activeTexture, textures: [...textures], framebuffer, viewport: Array.from(viewport), program, arrayBuffer,
      enabled: [...enabled].sort(), depthMask, colorMask: [...colorMask], blendFactors: [...blendFactors], blendEquations: [...blendEquations], depthFunc,
      attributes: [...attributes].map(([key, value]) => [key, { ...value }]) }),
  };
}

function view(): XrCameraViewLike {
  return {
    camera: { width: 960, height: 540 },
    projectionMatrix: new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]),
    transform: { matrix: new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]) },
  };
}

describe('RawCameraGuidanceCapture tag path', () => {
  afterEach(() => vi.restoreAllMocks());

  it('delivers top-left owned RGBA and matrices without invoking the generic guidance builder', () => {
    const width = 960; const height = 540; const source = new Uint8Array(width * height * 4);
    for (let index = 0; index < source.length; index += 1) source[index] = index % 251;
    const fake = fakeGl(source); const capture = new RawCameraGuidanceCapture(fake.gl, width, height);
    const initialState = fake.state(); const builder = vi.spyOn(rgbGuidance, 'buildRgbDepthGuidanceFrame');
    const input = view(); const binding = { getCameraImage: () => ({ id: 'camera-texture' } as WebGLTexture) };
    const frame = capture.captureTag(binding, input, 441);

    expect(builder).not.toHaveBeenCalled();
    expect(frame).not.toBeNull();
    expect(frame).toMatchObject({ width, height, capturedAtMs: 441, rgbaOwnership: 'owned' });
    expect(frame!.rgba.byteOffset).toBe(0); expect(frame!.rgba.byteLength).toBe(width * height * 4);
    const oldOutput = rgbGuidance.buildRgbDepthGuidanceFrame(source, width, height, width, height, 'bottom-left', 441).rgba!;
    expect(frame!.rgba).toEqual(oldOutput);
    expect(Array.from(frame!.projectionMatrix)).toEqual(Array.from(input.projectionMatrix!));
    expect(Array.from(frame!.worldFromView)).toEqual(Array.from(input.transform!.matrix));
    expect(fake.drawRasterStates).toEqual([{ blend: false, depth: false, scissor: false, cull: false, stencil: false, colorMask: [true, true, true, true], depthMask: true }]);
    expect(fake.state()).toEqual(initialState);
    capture.dispose();
  });

  it('releases all high-resolution capture resources on repeated disable cycles', () => {
    const fake = fakeGl(new Uint8Array(16));
    for (let cycle = 0; cycle < 5; cycle += 1) {
      const capture = new RawCameraGuidanceCapture(fake.gl, 2, 2);
      capture.captureTag({ getCameraImage: () => ({ id: `camera-${cycle}` } as WebGLTexture) }, view(), cycle);
      capture.dispose(); capture.dispose();
    }
    const createdCounts = fake.created.reduce<Record<string, number>>((counts, item) => { counts[item.kind] = (counts[item.kind] ?? 0) + 1; return counts; }, {});
    const deletedCounts = fake.deleted.reduce<Record<string, number>>((counts, kind) => { counts[kind] = (counts[kind] ?? 0) + 1; return counts; }, {});
    expect(deletedCounts).toEqual(createdCounts);
  });

  it('restores coverage draw state before the next camera copy on the same context', () => {
    const fake = fakeGl(new Uint8Array(16));
    const initialState = fake.state();
    const renderer = new LiveCoveragePointRenderer(fake.gl);
    renderer.update({ positions: new Float32Array([1, 2, 3]), confidence: new Float32Array([0.8]), pointCount: 1 });
    renderer.render({ framebuffer: null, getViewport: () => ({ x: 0, y: 0, width: 8, height: 8 }) }, [{
      projectionMatrix: new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]),
      transform: { matrix: new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]) },
    }]);
    expect(fake.state()).toEqual(initialState);

    const cameraDrawIndex = fake.drawRasterStates.length;
    const capture = new RawCameraGuidanceCapture(fake.gl, 2, 2);
    const beforeCameraCopy = fake.state();
    const frame = capture.captureTag({ getCameraImage: () => ({ id: 'shared-camera' } as WebGLTexture) }, view(), 12);
    expect(frame).not.toBeNull();
    expect(fake.drawRasterStates[cameraDrawIndex]).toEqual({ blend: false, depth: false, scissor: false, cull: false, stencil: false, colorMask: [true, true, true, true], depthMask: true });
    expect(fake.state()).toEqual(beforeCameraCopy);
    capture.dispose();
    renderer.dispose();
  });

  it('makes one owned RGBA copy and transfers that buffer directly to the detector', async () => {
    const fake = fakeGl(Uint8Array.from([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]));
    const capture = new RawCameraGuidanceCapture(fake.gl, 2, 2);
    const frame = capture.captureTag({ getCameraImage: () => ({ id: 'copy-budget-camera' } as WebGLTexture) }, view(), 13)!;
    expect(fake.readbackTargets).toHaveLength(1);
    expect(frame.rgba.buffer).not.toBe(fake.readbackTargets[0].buffer);

    const listeners = new Map<string, Set<(event: any) => void>>();
    const messages: { message: any; transfer?: Transferable[] }[] = [];
    const worker = {
      addEventListener(type: string, listener: (event: any) => void) { const bucket = listeners.get(type) ?? new Set(); bucket.add(listener); listeners.set(type, bucket); },
      removeEventListener(type: string, listener: (event: any) => void) { listeners.get(type)?.delete(listener); },
      postMessage(message: any, transfer?: Transferable[]) { messages.push({ message, transfer }); },
      terminate() {},
    };
    const client = new TagDetectorClient(() => worker as never);
    const pending = client.detect(frame);
    const request = messages.find((item) => item.message.type === 'detect')!;
    expect(request.message.rgba).toBe(frame.rgba);
    expect(request.transfer).toEqual([frame.rgba.buffer]);
    for (const listener of listeners.get('message') ?? []) listener({ data: { type: 'result', requestId: request.message.requestId, epoch: request.message.epoch, detections: [] } });
    await expect(pending).resolves.toMatchObject({ detections: [] });
    client.dispose();
    capture.dispose();
  });
});
