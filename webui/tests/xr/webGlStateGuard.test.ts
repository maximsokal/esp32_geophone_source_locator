import { describe, expect, it } from 'vitest';

import { captureWebGlRasterState, captureWebGlState, restoreWebGlRasterState, restoreWebGlState, setWebGlCopyRasterState } from '../../src/xr/webGlStateGuard';

interface FakeState {
  activeTexture: number;
  textures: Map<number, object | null>;
  framebuffer: object | null;
  readFramebuffer: object | null;
  drawFramebuffer: object | null;
  viewport: Int32Array;
  program: object | null;
  arrayBuffer: object | null;
  pixelPackBuffer: object | null;
  vao: object | null;
  attribEnabled: boolean;
  attribBuffer: object | null;
  attribPointer: { size: number; type: number; normalized: boolean; stride: number; offset: number };
  enabled: Set<number>;
  blendFactors: [number, number, number, number];
  blendEquations: [number, number];
  depthFunc: number;
  depthMask: boolean;
  scissorBox: Int32Array;
  colorMask: [boolean, boolean, boolean, boolean];
}

function fakeGl(webgl2: boolean) {
  const constants = {
    ACTIVE_TEXTURE: 1,
    TEXTURE_BINDING_2D: 2,
    FRAMEBUFFER_BINDING: 3,
    VIEWPORT: 4,
    CURRENT_PROGRAM: 5,
    ARRAY_BUFFER_BINDING: 6,
    TEXTURE0: 100,
    TEXTURE_2D: 101,
    ARRAY_BUFFER: 102,
    FRAMEBUFFER: 103,
    VERTEX_ATTRIB_ARRAY_ENABLED: 104,
    VERTEX_ATTRIB_ARRAY_BUFFER_BINDING: 105,
    VERTEX_ATTRIB_ARRAY_SIZE: 106,
    VERTEX_ATTRIB_ARRAY_TYPE: 107,
    VERTEX_ATTRIB_ARRAY_NORMALIZED: 108,
    VERTEX_ATTRIB_ARRAY_STRIDE: 109,
    VERTEX_ATTRIB_ARRAY_POINTER: 110,
    READ_FRAMEBUFFER_BINDING: 111,
    DRAW_FRAMEBUFFER_BINDING: 112,
    VERTEX_ARRAY_BINDING: 113,
    READ_FRAMEBUFFER: 114,
    DRAW_FRAMEBUFFER: 115,
    PIXEL_PACK_BUFFER: 116,
    PIXEL_PACK_BUFFER_BINDING: 117,
    BLEND: 118, DEPTH_TEST: 119, SCISSOR_TEST: 120, CULL_FACE: 121, STENCIL_TEST: 122,
    COLOR_WRITEMASK: 123, DEPTH_WRITEMASK: 124, BLEND_SRC_RGB: 125, BLEND_DST_RGB: 126,
    BLEND_SRC_ALPHA: 127, BLEND_DST_ALPHA: 128, BLEND_EQUATION_RGB: 129, BLEND_EQUATION_ALPHA: 130,
    DEPTH_FUNC: 131, SCISSOR_BOX: 132,
  };
  const state: FakeState = {
    activeTexture: 103,
    textures: new Map([[100, { id: 'tex0' }], [103, { id: 'tex3' }]]),
    framebuffer: { id: 'fb' },
    readFramebuffer: { id: 'read' },
    drawFramebuffer: { id: 'draw' },
    viewport: new Int32Array([7, 8, 640, 480]),
    program: { id: 'program' },
    arrayBuffer: { id: 'array' },
    pixelPackBuffer: { id: 'pack' },
    vao: { id: 'vao' },
    attribEnabled: false,
    attribBuffer: { id: 'attrib-buffer' },
    attribPointer: { size: 3, type: 5126, normalized: true, stride: 20, offset: 8 },
    enabled: new Set([constants.BLEND, constants.DEPTH_TEST, constants.SCISSOR_TEST, constants.CULL_FACE, constants.STENCIL_TEST]),
    blendFactors: [770, 771, 1, 0], blendEquations: [32774, 32778], depthFunc: 515, depthMask: false,
    scissorBox: new Int32Array([9, 7, 32, 24]), colorMask: [true, false, true, false],
  };

  const gl: Record<string, unknown> = {
    ...constants,
    getParameter(parameter: number) {
      switch (parameter) {
        case constants.ACTIVE_TEXTURE: return state.activeTexture;
        case constants.TEXTURE_BINDING_2D: return state.textures.get(state.activeTexture) ?? null;
        case constants.FRAMEBUFFER_BINDING: return state.framebuffer;
        case constants.READ_FRAMEBUFFER_BINDING: return state.readFramebuffer;
        case constants.DRAW_FRAMEBUFFER_BINDING: return state.drawFramebuffer;
        case constants.VIEWPORT: return state.viewport;
        case constants.CURRENT_PROGRAM: return state.program;
        case constants.ARRAY_BUFFER_BINDING: return state.arrayBuffer;
        case constants.PIXEL_PACK_BUFFER_BINDING: return state.pixelPackBuffer;
        case constants.COLOR_WRITEMASK: return state.colorMask;
        case constants.DEPTH_WRITEMASK: return state.depthMask;
        case constants.BLEND_SRC_RGB: return state.blendFactors[0];
        case constants.BLEND_DST_RGB: return state.blendFactors[1];
        case constants.BLEND_SRC_ALPHA: return state.blendFactors[2];
        case constants.BLEND_DST_ALPHA: return state.blendFactors[3];
        case constants.BLEND_EQUATION_RGB: return state.blendEquations[0];
        case constants.BLEND_EQUATION_ALPHA: return state.blendEquations[1];
        case constants.DEPTH_FUNC: return state.depthFunc;
        case constants.SCISSOR_BOX: return state.scissorBox;
        case constants.VERTEX_ARRAY_BINDING: return state.vao;
        default: return null;
      }
    },
    activeTexture(unit: number) { state.activeTexture = unit; },
    bindTexture(_target: number, texture: object | null) { state.textures.set(state.activeTexture, texture); },
    bindFramebuffer(target: number, framebuffer: object | null) {
      if (webgl2 && target === constants.READ_FRAMEBUFFER) state.readFramebuffer = framebuffer;
      else if (webgl2 && target === constants.DRAW_FRAMEBUFFER) state.drawFramebuffer = framebuffer;
      else state.framebuffer = framebuffer;
    },
    bindBuffer(target: number, buffer: object | null) {
      if (webgl2 && target === constants.PIXEL_PACK_BUFFER) state.pixelPackBuffer = buffer;
      else state.arrayBuffer = buffer;
    },
    useProgram(program: object | null) { state.program = program; },
    viewport(x: number, y: number, width: number, height: number) { state.viewport = new Int32Array([x, y, width, height]); },
    getVertexAttrib(_location: number, parameter: number) {
      switch (parameter) {
        case constants.VERTEX_ATTRIB_ARRAY_ENABLED: return state.attribEnabled;
        case constants.VERTEX_ATTRIB_ARRAY_BUFFER_BINDING: return state.attribBuffer;
        case constants.VERTEX_ATTRIB_ARRAY_SIZE: return state.attribPointer.size;
        case constants.VERTEX_ATTRIB_ARRAY_TYPE: return state.attribPointer.type;
        case constants.VERTEX_ATTRIB_ARRAY_NORMALIZED: return state.attribPointer.normalized;
        case constants.VERTEX_ATTRIB_ARRAY_STRIDE: return state.attribPointer.stride;
        default: return null;
      }
    },
    getVertexAttribOffset() { return state.attribPointer.offset; },
    vertexAttribPointer(_location: number, size: number, type: number, normalized: boolean, stride: number, offset: number) {
      state.attribBuffer = state.arrayBuffer;
      state.attribPointer = { size, type, normalized, stride, offset };
    },
    enableVertexAttribArray() { state.attribEnabled = true; },
    disableVertexAttribArray() { state.attribEnabled = false; },
    isEnabled(capability: number) { return state.enabled.has(capability); },
    enable(capability: number) { state.enabled.add(capability); },
    disable(capability: number) { state.enabled.delete(capability); },
    colorMask(red: boolean, green: boolean, blue: boolean, alpha: boolean) { state.colorMask = [red, green, blue, alpha]; },
    depthMask(value: boolean) { state.depthMask = value; },
    depthFunc(value: number) { state.depthFunc = value; },
    scissor(x: number, y: number, width: number, height: number) { state.scissorBox = new Int32Array([x, y, width, height]); },
    blendFuncSeparate(srcRgb: number, dstRgb: number, srcAlpha: number, dstAlpha: number) { state.blendFactors = [srcRgb, dstRgb, srcAlpha, dstAlpha]; },
    blendEquationSeparate(rgb: number, alpha: number) { state.blendEquations = [rgb, alpha]; },
  };
  if (webgl2) {
    gl.bindVertexArray = (vao: object | null) => { state.vao = vao; };
  }
  return { gl: gl as unknown as WebGLRenderingContext, state, constants };
}

function disturb(state: FakeState): void {
  state.activeTexture = 100;
  state.textures.set(100, { id: 'camera' });
  state.framebuffer = { id: 'capture' };
  state.readFramebuffer = { id: 'capture-read' };
  state.drawFramebuffer = { id: 'capture-draw' };
  state.viewport = new Int32Array([0, 0, 256, 144]);
  state.program = { id: 'capture-program' };
  state.arrayBuffer = { id: 'capture-array' };
  state.pixelPackBuffer = { id: 'capture-pack' };
  state.vao = { id: 'capture-vao' };
  state.attribEnabled = true;
  state.attribBuffer = { id: 'capture-attrib' };
  state.attribPointer = { size: 2, type: 5126, normalized: false, stride: 0, offset: 0 };
  state.enabled = new Set();
  state.blendFactors = [1, 0, 1, 0]; state.blendEquations = [32774, 32774]; state.depthFunc = 515; state.depthMask = true;
  state.scissorBox = new Int32Array([0, 0, 8, 8]); state.colorMask = [false, true, false, true];
}

describe('raw-camera WebGL state guard', () => {
  it('uses explicit exact-copy raster state and restores dirty caller values exactly', () => {
    const { gl, state, constants } = fakeGl(false);
    const snapshot = captureWebGlRasterState(gl);

    setWebGlCopyRasterState(gl);

    expect(state.enabled.has(constants.BLEND)).toBe(false);
    expect(state.enabled.has(constants.DEPTH_TEST)).toBe(false);
    expect(state.enabled.has(constants.SCISSOR_TEST)).toBe(false);
    expect(state.enabled.has(constants.STENCIL_TEST)).toBe(false);
    expect(state.enabled.has(constants.CULL_FACE)).toBe(false);
    expect(state.colorMask).toEqual([true, true, true, true]);
    expect(state.depthMask).toBe(true);

    restoreWebGlRasterState(gl, snapshot);
    expect(state.enabled).toEqual(new Set([constants.BLEND, constants.DEPTH_TEST, constants.SCISSOR_TEST, constants.CULL_FACE, constants.STENCIL_TEST]));
    expect(state.colorMask).toEqual([true, false, true, false]);
    expect(state.depthMask).toBe(false);
  });

  it('restores WebGL1 texture units, framebuffer and vertex attribute state', () => {
    const { gl, state } = fakeGl(false);
    const originalTexture0 = state.textures.get(100);
    const originalTexture3 = state.textures.get(103);
    const snapshot = captureWebGlState(gl, 2);
    disturb(state);

    restoreWebGlState(gl, 2, snapshot);

    expect(state.activeTexture).toBe(103);
    expect(state.textures.get(100)).toBe(originalTexture0);
    expect(state.textures.get(103)).toBe(originalTexture3);
    expect((state.framebuffer as { id: string }).id).toBe('fb');
    expect(Array.from(state.viewport)).toEqual([7, 8, 640, 480]);
    expect((state.program as { id: string }).id).toBe('program');
    expect((state.arrayBuffer as { id: string }).id).toBe('array');
    expect(state.attribEnabled).toBe(false);
    expect((state.attribBuffer as { id: string }).id).toBe('attrib-buffer');
    expect(state.attribPointer).toEqual({ size: 3, type: 5126, normalized: true, stride: 20, offset: 8 });
  });

  it('restores independent WebGL2 READ/DRAW framebuffers, VAO and pixel-pack buffer', () => {
    const { gl, state } = fakeGl(true);
    const snapshot = captureWebGlState(gl, 2);
    disturb(state);

    restoreWebGlState(gl, 2, snapshot);

    expect((state.readFramebuffer as { id: string }).id).toBe('read');
    expect((state.drawFramebuffer as { id: string }).id).toBe('draw');
    expect((state.vao as { id: string }).id).toBe('vao');
    expect((state.pixelPackBuffer as { id: string }).id).toBe('pack');
  });
});
