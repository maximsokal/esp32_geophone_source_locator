import { describe, expect, it } from 'vitest';

import { applyCoverageRenderBudget, LiveCoveragePointRenderer } from '../../src/xr/liveCoverageRenderer';
import { captureWebGlRasterState, restoreWebGlRasterState, setWebGlCopyRasterState } from '../../src/xr/webGlStateGuard';

function snapshot(regular: number, manual = 0) {
  const count = regular + manual;
  const positions = new Float32Array(count * 3);
  const confidence = new Float32Array(count);
  for (let index = 0; index < count; index += 1) {
    positions[index * 3] = index;
    positions[index * 3 + 1] = index + 0.25;
    positions[index * 3 + 2] = index + 0.5;
    confidence[index] = index < regular ? 0.2 + (index % 7) * 0.1 : 2;
  }
  return { positions, confidence, pointCount: count, manualPointCount: manual };
}

describe('applyCoverageRenderBudget', () => {
  it('does not touch full-detail snapshots below the budget', () => {
    const input = snapshot(100, 2);
    const output = applyCoverageRenderBudget(input, 12000);
    expect(output.pointCount).toBe(102);
    expect(output.manualPointCount).toBe(2);
    expect(Array.from(output.positions)).toEqual(Array.from(input.positions));
  });

  it('downsamples only regular live points and always preserves manual pins', () => {
    const input = snapshot(12000, 3);
    const output = applyCoverageRenderBudget(input, 4800);
    expect(output.pointCount).toBe(4803);
    expect(output.manualPointCount).toBe(3);
    for (let manual = 0; manual < 3; manual += 1) {
      const target = 4800 + manual;
      const source = 12000 + manual;
      expect(output.positions[target * 3]).toBe(input.positions[source * 3]);
      expect(output.confidence[target]).toBe(2);
    }
  });

  it('is deterministic so pressure changes cannot reshuffle the same LOD randomly', () => {
    const input = snapshot(1000);
    const a = applyCoverageRenderBudget(input, 137);
    const b = applyCoverageRenderBudget(input, 137);
    expect(Array.from(a.positions)).toEqual(Array.from(b.positions));
    expect(Array.from(a.confidence)).toEqual(Array.from(b.confidence));
  });

  it('supports a zero regular-point budget without deleting operator pins', () => {
    const input = snapshot(30, 2);
    const output = applyCoverageRenderBudget(input, 0);
    expect(output.pointCount).toBe(2);
    expect(output.manualPointCount).toBe(2);
    expect(Array.from(output.confidence)).toEqual([2, 2]);
  });
});

function dirtyRendererGl() {
  const C={ACTIVE_TEXTURE:1,TEXTURE_BINDING_2D:2,FRAMEBUFFER_BINDING:3,VIEWPORT:4,CURRENT_PROGRAM:5,ARRAY_BUFFER_BINDING:6,
    TEXTURE0:100,TEXTURE_2D:101,ARRAY_BUFFER:102,FRAMEBUFFER:103,BLEND:110,DEPTH_TEST:111,SCISSOR_TEST:112,CULL_FACE:113,STENCIL_TEST:114,
    COLOR_WRITEMASK:115,DEPTH_WRITEMASK:116,BLEND_SRC_RGB:117,BLEND_DST_RGB:118,BLEND_SRC_ALPHA:119,BLEND_DST_ALPHA:120,
    BLEND_EQUATION_RGB:121,BLEND_EQUATION_ALPHA:122,DEPTH_FUNC:123,VERTEX_ATTRIB_ARRAY_ENABLED:124,VERTEX_ATTRIB_ARRAY_BUFFER_BINDING:125,
    VERTEX_ATTRIB_ARRAY_SIZE:126,VERTEX_ATTRIB_ARRAY_TYPE:127,VERTEX_ATTRIB_ARRAY_NORMALIZED:128,VERTEX_ATTRIB_ARRAY_STRIDE:129,
    VERTEX_ATTRIB_ARRAY_POINTER:130,VERTEX_SHADER:200,FRAGMENT_SHADER:201,COMPILE_STATUS:202,LINK_STATUS:203,FLOAT:204,DYNAMIC_DRAW:205,
    SRC_ALPHA:206,ONE_MINUS_SRC_ALPHA:207,FUNC_ADD:208,LEQUAL:209,POINTS:210};
  const enabled=new Set<number>([C.BLEND,C.DEPTH_TEST,C.SCISSOR_TEST,C.CULL_FACE,C.STENCIL_TEST]);
  let blendFactors:[number,number,number,number]=[21,22,23,24];let blendEquations:[number,number]=[25,26];
  let depthFunc=27,depthMask=false,colorMask:[boolean,boolean,boolean,boolean]=[true,false,true,false];
  let viewport=new Int32Array([9,8,100,90]);let program:object|null={id:'prior-program'};let arrayBuffer:object|null={id:'prior-array'};
  let activeTexture=C.TEXTURE0;let framebuffer:object|null={id:'prior-framebuffer'};let drawCount=0;
  const attributes=new Map<number,{enabled:boolean;buffer:object|null;size:number;type:number;normalized:boolean;stride:number;offset:number}>([
    [0,{enabled:true,buffer:{id:'prior-position'},size:2,type:C.FLOAT,normalized:true,stride:16,offset:4}],
    [1,{enabled:false,buffer:{id:'prior-confidence'},size:2,type:C.FLOAT,normalized:false,stride:8,offset:2}],
  ]);
  const gl:any={...C,
    getParameter(parameter:number){switch(parameter){case C.ACTIVE_TEXTURE:return activeTexture;case C.TEXTURE_BINDING_2D:return null;case C.FRAMEBUFFER_BINDING:return framebuffer;case C.VIEWPORT:return viewport;case C.CURRENT_PROGRAM:return program;case C.ARRAY_BUFFER_BINDING:return arrayBuffer;case C.COLOR_WRITEMASK:return colorMask;case C.DEPTH_WRITEMASK:return depthMask;case C.BLEND_SRC_RGB:return blendFactors[0];case C.BLEND_DST_RGB:return blendFactors[1];case C.BLEND_SRC_ALPHA:return blendFactors[2];case C.BLEND_DST_ALPHA:return blendFactors[3];case C.BLEND_EQUATION_RGB:return blendEquations[0];case C.BLEND_EQUATION_ALPHA:return blendEquations[1];case C.DEPTH_FUNC:return depthFunc;default:return null;}},
    isEnabled(capability:number){return enabled.has(capability);},enable(capability:number){enabled.add(capability);},disable(capability:number){enabled.delete(capability);},
    colorMask(r:boolean,g:boolean,b:boolean,a:boolean){colorMask=[r,g,b,a];},depthMask(value:boolean){depthMask=value;},depthFunc(value:number){depthFunc=value;},
    blendFunc(src:number,dst:number){blendFactors=[src,dst,src,dst];},blendFuncSeparate(a:number,b:number,c:number,d:number){blendFactors=[a,b,c,d];},
    blendEquationSeparate(a:number,b:number){blendEquations=[a,b];},activeTexture(value:number){activeTexture=value;},bindTexture(){},
    bindFramebuffer(_target:number,value:object|null){framebuffer=value;},viewport(x:number,y:number,w:number,h:number){viewport=new Int32Array([x,y,w,h]);},
    useProgram(value:object|null){program=value;},bindBuffer(_target:number,value:object|null){arrayBuffer=value;},bufferData(){},createBuffer(){return{};},deleteBuffer(){},
    createShader(){return{};},shaderSource(){},compileShader(){},getShaderParameter(){return true;},getShaderInfoLog(){return'';},deleteShader(){},
    createProgram(){return{};},attachShader(){},linkProgram(){},getProgramParameter(){return true;},getProgramInfoLog(){return'';},deleteProgram(){},
    getAttribLocation(_program:unknown,name:string){return name==='a_position'?0:1;},getUniformLocation(){return{};},uniformMatrix4fv(){},
    getVertexAttrib(location:number,parameter:number){const a=attributes.get(location)!;switch(parameter){case C.VERTEX_ATTRIB_ARRAY_ENABLED:return a.enabled;case C.VERTEX_ATTRIB_ARRAY_BUFFER_BINDING:return a.buffer;case C.VERTEX_ATTRIB_ARRAY_SIZE:return a.size;case C.VERTEX_ATTRIB_ARRAY_TYPE:return a.type;case C.VERTEX_ATTRIB_ARRAY_NORMALIZED:return a.normalized;case C.VERTEX_ATTRIB_ARRAY_STRIDE:return a.stride;default:return null;}},
    getVertexAttribOffset(location:number){return attributes.get(location)!.offset;},enableVertexAttribArray(location:number){attributes.get(location)!.enabled=true;},disableVertexAttribArray(location:number){attributes.get(location)!.enabled=false;},
    vertexAttribPointer(location:number,size:number,type:number,normalized:boolean,stride:number,offset:number){attributes.set(location,{...attributes.get(location)!,buffer:arrayBuffer,size,type,normalized,stride,offset});},
    drawArrays(){drawCount+=1;},
  };
  return{gl,enabled,attributes,getState:()=>({blendFactors:[...blendFactors],blendEquations:[...blendEquations],depthFunc,depthMask,colorMask:[...colorMask],viewport:Array.from(viewport),program,arrayBuffer,enabled:[...enabled].sort(),attributes:[...attributes].map(([key,value])=>[key,{...value}])}),get drawCount(){return drawCount;}};
}

describe('LiveCoveragePointRenderer WebGL state',()=>{
  it('restores dirty coverage state before the next camera copy on the shared context',()=>{
    const fake=dirtyRendererGl();const renderer=new LiveCoveragePointRenderer(fake.gl);
    renderer.update(snapshot(3,1));
    const before=fake.getState();
    renderer.render({getViewport:()=>({x:0,y:0,width:64,height:64})},[{
      projectionMatrix:new Float32Array([1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]),
      transform:{matrix:new Float32Array([1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1])},
    }]);
    expect(fake.drawCount).toBe(2);
    expect(fake.getState()).toEqual(before);
    const cameraRaster=captureWebGlRasterState(fake.gl);
    setWebGlCopyRasterState(fake.gl);
    expect(fake.enabled.has(fake.gl.BLEND)).toBe(false);
    restoreWebGlRasterState(fake.gl,cameraRaster);
    expect(fake.getState()).toEqual(before);
    renderer.dispose();
  });
});
