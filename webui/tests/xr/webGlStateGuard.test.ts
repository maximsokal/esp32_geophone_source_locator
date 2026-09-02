import { describe, expect, it } from 'vitest';

import { captureWebGlState, restoreWebGlState } from '../../src/xr/webGlStateGuard';

interface FakeState {
  activeTexture: number;
  textures: Map<number, object | null>;
  framebuffer: object | null;
  readFramebuffer: object | null;
  drawFramebuffer: object | null;
  viewport: Int32Array;
  program: object | null;
  arrayBuffer: object | null;
  vao: object | null;
  attribEnabled: boolean;
  attribBuffer: object | null;
  attribPointer: { size: number; type: number; normalized: boolean; stride: number; offset: number };
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
    vao: { id: 'vao' },
    attribEnabled: false,
    attribBuffer: { id: 'attrib-buffer' },
    attribPointer: { size: 3, type: 5126, normalized: true, stride: 20, offset: 8 },
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
    bindBuffer(_target: number, buffer: object | null) { state.arrayBuffer = buffer; },
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
  state.vao = { id: 'capture-vao' };
  state.attribEnabled = true;
  state.attribBuffer = { id: 'capture-attrib' };
  state.attribPointer = { size: 2, type: 5126, normalized: false, stride: 0, offset: 0 };
}

describe('raw-camera WebGL state guard', () => {
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

  it('restores independent WebGL2 READ/DRAW framebuffers and VAO', () => {
    const { gl, state } = fakeGl(true);
    const snapshot = captureWebGlState(gl, 2);
    disturb(state);

    restoreWebGlState(gl, 2, snapshot);

    expect((state.readFramebuffer as { id: string }).id).toBe('read');
    expect((state.drawFramebuffer as { id: string }).id).toBe('draw');
    expect((state.vao as { id: string }).id).toBe('vao');
  });
});
