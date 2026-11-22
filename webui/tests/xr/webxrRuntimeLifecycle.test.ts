import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { DepthPointCloudAccumulator } from '../../src/xr/depthPointCloud';
import { WebXrRuntime } from '../../src/xr/webxrRuntime';
import { LIVE_MESH_MIN_INTERVAL_MS } from '../../src/xr/liveMeshClient';
import { clientDiagnostics } from '../../src/data/clientDiagnostics';
import type { TagCameraFrame } from '../../src/xr/fiducial/tagDetectorClient';
import { activeXrPerformancePressure, activeXrRenderPolicy, recordActiveXrPerformanceMetric } from '../../src/xr/xrPerformanceProfiler';
import { deferred, FakeWakeLock, FakeXrLayer, FakeXrSession, FakeXrSystem } from './support/xrFakes';

const runtimes:WebXrRuntime[]=[];
let xr:FakeXrSystem;
let wakeLock:FakeWakeLock;
let gl:{getExtension:()=>{loseContext:()=>void}|null;makeXRCompatible?:()=>Promise<void>;canvas:EventTarget;FRAMEBUFFER:number;COLOR_BUFFER_BIT:number;DEPTH_BUFFER_BIT:number;bindFramebuffer:()=>void;viewport:()=>void;clearColor:()=>void;clearDepth:()=>void;clear:()=>void};
let glContextsCreated=0;let glContextsLost=0;
let throwOnGlClear=false;
type StubListener=EventListenerOrEventListenerObject;
interface StubDocument{
  visibilityState:string;
  body:object;
  documentElement:{classList:{add:(name:string)=>void;remove:(...names:string[])=>void;contains:(name:string)=>boolean}};
  addEventListener:(type:string,listener:StubListener|null)=>void;
  removeEventListener:(type:string,listener:StubListener|null)=>void;
  dispatchEvent:(event:Event)=>boolean;
  createElement:(name:string)=>{getContext:(kind:string,attributes?:unknown)=>unknown|null};
}
let browserDocument:StubDocument;let browserCanvas:EventTarget;

function dispatchDocument(type:string):void{
  browserDocument.dispatchEvent({type} as Event);
}

function setVisibility(value:string):void{
  browserDocument.visibilityState=value;
  dispatchDocument('visibilitychange');
}

function installEnvironment(makeCompatible?:()=>Promise<void>):void{
  xr=new FakeXrSystem();wakeLock=new FakeWakeLock();glContextsCreated=0;glContextsLost=0;throwOnGlClear=false;
  browserCanvas=new EventTarget();
  gl={canvas:browserCanvas,FRAMEBUFFER:1,COLOR_BUFFER_BIT:2,DEPTH_BUFFER_BIT:4,bindFramebuffer:()=>undefined,viewport:()=>undefined,clearColor:()=>undefined,clearDepth:()=>undefined,clear:()=>{if(throwOnGlClear)throw new Error('XR clear failed.');},getExtension:()=>({loseContext:()=>{glContextsLost+=1;}}),makeXRCompatible:makeCompatible??(()=>Promise.resolve())};
  const listeners=new Map<string,Set<StubListener>>();
  const classes=new Set<string>();
  browserDocument={
    visibilityState:'visible',
    body:{},
    documentElement:{classList:{
      add:(name)=>{classes.add(name);},
      remove:(...names)=>{for(const name of names)classes.delete(name);},
      contains:(name)=>classes.has(name),
    }},
    addEventListener:(type,listener)=>{
      if(listener===null)return;
      const bucket=listeners.get(type)??new Set<StubListener>();
      bucket.add(listener);
      listeners.set(type,bucket);
    },
    removeEventListener:(type,listener)=>{
      if(listener===null)return;
      listeners.get(type)?.delete(listener);
    },
    dispatchEvent:(event)=>{
      for(const listener of [...(listeners.get(event.type)??[])]){
        if(typeof listener==='function')listener.call(browserDocument,event);
        else listener.handleEvent(event);
      }
      return true;
    },
    createElement:()=>Object.assign(browserCanvas,{
      getContext:(kind:string)=>{
        if(kind!=='webgl2'&&kind!=='webgl')return null;
        glContextsCreated+=1;
        return gl;
      },
    }),
  };
  let nextTimerId=0;
  vi.stubGlobal('window',{
    isSecureContext:true,
    XRWebGLLayer:FakeXrLayer,
    localStorage:{getItem:()=>null,setItem:()=>undefined},
    setTimeout:()=>++nextTimerId,
    clearTimeout:()=>undefined,
    setInterval:()=>++nextTimerId,
    clearInterval:()=>undefined,
  });
  vi.stubGlobal('document',browserDocument);
  vi.stubGlobal('navigator',{xr,wakeLock});
}

function createRuntime():WebXrRuntime{const runtime=new WebXrRuntime();runtimes.push(runtime);runtime.attach();return runtime;}
async function flushMicrotasks():Promise<void>{for(let i=0;i<8;i+=1)await Promise.resolve();}
async function until(check:()=>boolean):Promise<void>{for(let i=0;i<20&&!check();i+=1)await Promise.resolve();if(!check())throw new Error('Expected lifecycle operation did not reach its deferred boundary.');}
function emptyPose(){return{transform:{position:{x:0,y:0,z:0},orientation:{x:0,y:0,z:0,w:1},matrix:new Float32Array(16)},views:[]};}
function fakeFrame(session:FakeXrSession,getViewerPose:()=>unknown):never{return{session,getViewerPose} as never;}
function evidencePose(){
  const matrix=new Float32Array([1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]);
  const position={x:0,y:0,z:0};const orientation={x:0,y:0,z:0,w:1};
  const view={projectionMatrix:matrix,transform:{matrix,position,orientation},camera:{width:960,height:540}};
  return{transform:{matrix,position,orientation},views:[view]};
}
function evidenceFrame(session:FakeXrSession):never{
  const pose=evidencePose();const matrix=pose.transform.matrix;
  return{session,getViewerPose:()=>pose,getDepthInformation:()=>({width:64,height:48,projectionMatrix:matrix,transform:{matrix},getDepthInMeters:()=>1.5})} as never;
}
function fakeTagFrame(capturedAtMs:number):TagCameraFrame{
  return{width:1,height:1,rgba:new Uint8Array(4),capturedAtMs,cameraPosition:[0,0,0],cameraForward:[0,0,-1],projectionMatrix:new Float32Array(16),worldFromView:new Float32Array(16)};
}

beforeEach(()=>installEnvironment());
afterEach(async()=>{
  await Promise.all(runtimes.splice(0).map((runtime)=>runtime.dispose()));
  vi.restoreAllMocks();vi.unstubAllGlobals();
});

describe('WebXrRuntime lifecycle ownership',()=>{
  it.each(['opaque','alpha-blend','additive'])('restores the document after %s compositing',async(blend)=>{
    const session=new FakeXrSession();session.environmentBlendMode=blend;xr.queue(session);
    const runtime=createRuntime();await runtime.start();
    expect(document.documentElement.classList.contains('xr-session-active')).toBe(true);
    expect(document.documentElement.classList.contains('xr-passthrough-active')).toBe(blend!=='opaque');
    await runtime.stop();
    expect(document.documentElement.classList.contains('xr-session-active')).toBe(false);
    expect(document.documentElement.classList.contains('xr-passthrough-active')).toBe(false);
  });
  it('keeps construction pure and protects the active profiler from stale detach',async()=>{
    const addListener=vi.spyOn(document,'addEventListener');const removeListener=vi.spyOn(document,'removeEventListener');
    const policyBefore=activeXrRenderPolicy();const abandoned=new WebXrRuntime();runtimes.push(abandoned);
    const abandonedProfiler=(abandoned as unknown as {performanceProfiler:{snapshot:()=>{sampleCount:number}}}).performanceProfiler;
    expect(addListener.mock.calls.filter(([type])=>type==='visibilitychange')).toHaveLength(0);
    expect(activeXrRenderPolicy()).toBe(policyBefore);
    recordActiveXrPerformanceMetric('fusion',100);
    expect(abandonedProfiler.snapshot().sampleCount).toBe(0);
    await abandoned.dispose();
    expect(removeListener.mock.calls.filter(([type])=>type==='visibilitychange')).toHaveLength(0);

    const first=new WebXrRuntime();runtimes.push(first);first.attach();
    const firstProfiler=(first as unknown as {performanceProfiler:{record:(metric:'coverageDraw',durationMs:number)=>void}}).performanceProfiler;
    for(let index=0;index<16;index+=1)firstProfiler.record('coverageDraw',15);
    const firstPolicy=activeXrRenderPolicy();expect(firstPolicy.pointBudget).toBe(4800);
    const throwaway=new WebXrRuntime();runtimes.push(throwaway);
    expect(activeXrRenderPolicy()).toBe(firstPolicy);
    throwaway.attach();
    const secondProfiler=(throwaway as unknown as {performanceProfiler:{record:(metric:'coverageDraw',durationMs:number)=>void}}).performanceProfiler;
    for(let index=0;index<16;index+=1)secondProfiler.record('coverageDraw',15);
    const secondPolicy=activeXrRenderPolicy();expect(secondPolicy.pointBudget).toBe(4800);
    await first.dispose();
    expect(activeXrRenderPolicy()).toBe(secondPolicy);
    expect(activeXrPerformancePressure()).toBe('high');
    await throwaway.dispose();
    expect(activeXrRenderPolicy()).toEqual(policyBefore);
    expect(activeXrPerformancePressure()).toBe('low');
  });

  it('returns committed visibility listeners to baseline across 100 attach/dispose cycles',async()=>{
    const addListener=vi.spyOn(document,'addEventListener');const removeListener=vi.spyOn(document,'removeEventListener');
    for(let index=0;index<100;index+=1){const runtime=new WebXrRuntime();runtimes.push(runtime);runtime.attach();runtime.detach();runtime.detach();await runtime.dispose();}
    expect(addListener.mock.calls.filter(([type])=>type==='visibilitychange')).toHaveLength(100);
    expect(removeListener.mock.calls.filter(([type])=>type==='visibilitychange')).toHaveLength(100);
    expect(activeXrPerformancePressure()).toBe('low');
    expect(activeXrRenderPolicy()).toEqual({pointBudget:12000,drawIntervalMs:0});
  });

  it('detaches a StrictMode replay attachment when unmount races the prior cleanup',async()=>{
    const addListener=vi.spyOn(document,'addEventListener');const removeListener=vi.spyOn(document,'removeEventListener');
    const runtime=new WebXrRuntime();runtimes.push(runtime);runtime.attach();const firstDetach=runtime.detach();runtime.attach();const finalDetach=runtime.detach();
    await Promise.all([firstDetach,finalDetach]);
    expect(addListener.mock.calls.filter(([type])=>type==='visibilitychange')).toHaveLength(2);
    expect(removeListener.mock.calls.filter(([type])=>type==='visibilitychange')).toHaveLength(2);
    expect(activeXrPerformancePressure()).toBe('low');
    expect(activeXrRenderPolicy()).toEqual({pointBudget:12000,drawIntervalMs:0});
  });

  it('publishes the same pressure used by render policy and scheduler readers',async()=>{
    const session=new FakeXrSession();xr.queue(session);const runtime=createRuntime();await runtime.start();
    const profiler=(runtime as unknown as {performanceProfiler:{record:(metric:'coverageDraw',durationMs:number)=>void}}).performanceProfiler;
    for(let index=0;index<16;index+=1)profiler.record('coverageDraw',15);
    expect(activeXrRenderPolicy()).toEqual({pointBudget:4800,drawIntervalMs:32});
    session.driveNextFrame(16,fakeFrame(session,()=>emptyPose()));
    expect(runtime.getState().performance.pressure).toBe('high');
    expect(activeXrPerformancePressure()).toBe(runtime.getState().performance.pressure);
    expect(activeXrRenderPolicy()).toEqual({pointBudget:runtime.getState().performance.adaptiveCoveragePointBudget,drawIntervalMs:32});
  });

  it('copies a tag frame only for additional subscribers before transfer can detach the first',()=>{
    const runtime=createRuntime();let secondFrame:any=null;const original=Uint8Array.from([1,2,3,4]);
    runtime.subscribeTagFrames((frame)=>frame.rgba.fill(0));
    runtime.subscribeTagFrames((frame)=>{secondFrame=frame;});
    (runtime as unknown as {acceptTagFrame:(frame:any)=>void}).acceptTagFrame({width:1,height:1,rgba:original,rgbaOwnership:'owned',capturedAtMs:1,projectionMatrix:new Float32Array(16),worldFromView:new Float32Array(16)});
    expect(secondFrame.rgba).not.toBe(original);expect(Array.from(secondFrame.rgba)).toEqual([1,2,3,4]);
  });

  it('tears down once after a viewer-pose fault without queuing another RAF',async()=>{
    const session=new FakeXrSession();xr.queue(session);const runtime=createRuntime();await runtime.start();await flushMicrotasks();
    session.driveNextFrame(16,fakeFrame(session,()=>{throw new Error('viewer pose failed');}));
    await until(()=>session.endCalls===1);await flushMicrotasks();
    expect(session.rafRequests).toBe(1);expect(session.liveFrameCallbacks).toBe(0);expect(session.endCalls).toBe(1);
    expect(runtime.getState().phase).toBe('error');expect(runtime.getState().error).toContain('viewer-pose');
    expect(wakeLock.liveSentinels.size).toBe(0);expect(glContextsCreated).toBe(glContextsLost);
    expect(document.documentElement.classList.contains('xr-session-active')).toBe(false);
  });

  it('tears down on a core WebGL clear fault before another frame is scheduled',async()=>{
    const session=new FakeXrSession();xr.queue(session);const runtime=createRuntime();await runtime.start();throwOnGlClear=true;
    session.driveNextFrame(16,fakeFrame(session,()=>emptyPose()));
    await until(()=>session.endCalls===1);await flushMicrotasks();
    expect(session.rafRequests).toBe(1);expect(session.liveFrameCallbacks).toBe(0);expect(runtime.getState().phase).toBe('error');
    expect(runtime.getState().error).toContain('xr-core-gl');expect(glContextsCreated).toBe(glContextsLost);
  });

  it('disables a failed coverage renderer but keeps depth capture and the frame loop alive',async()=>{
    const session=new FakeXrSession();xr.queue(session);const runtime=createRuntime();await runtime.start();
    const failingRenderer={render:vi.fn(()=>{throw new Error('coverage draw failed');}),dispose:vi.fn()};
    (runtime as unknown as {coverageRenderer:typeof failingRenderer}).coverageRenderer=failingRenderer;
    const captureDepth=vi.spyOn(runtime as unknown as {captureDepth:(time:number,frame:unknown,pose:unknown)=>void},'captureDepth');
    session.driveNextFrame(16,fakeFrame(session,()=>emptyPose()));
    await flushMicrotasks();
    expect(failingRenderer.render).toHaveBeenCalledTimes(1);expect(failingRenderer.dispose).toHaveBeenCalledTimes(1);
    expect(captureDepth).toHaveBeenCalledTimes(1);expect(runtime.getState().coverageGuideError).toContain('coverage draw failed');
    expect(runtime.getState().phase).toBe('running');expect(session.endCalls).toBe(0);expect(session.liveFrameCallbacks).toBe(1);
    session.driveNextFrame(32,fakeFrame(session,()=>emptyPose()));await flushMicrotasks();
    expect(failingRenderer.render).toHaveBeenCalledTimes(1);expect(captureDepth).toHaveBeenCalledTimes(2);expect(session.liveFrameCallbacks).toBe(1);
  });

  describe('the translucent live mesh over the camera',()=>{
    interface MeshInternals{
      liveMeshRenderer:unknown;liveMeshClient:unknown;liveMeshPending:unknown;liveMeshOff:boolean;livePerformancePressure:()=>string;
    }
    /** A started runtime whose mesh client/renderer are replaced by recorders; `result` is what the next worker reply would be. */
    async function meshRuntime(mode:'auto'|'on'|'off',pressure='low'){
      const session=new FakeXrSession();xr.queue(session);const runtime=createRuntime();await runtime.start();
      const internals=runtime as unknown as MeshInternals;
      const requests:number[]=[];
      const client={request:vi.fn((build:()=>unknown,_minIntervalMs?:number)=>{requests.push(1);return build!==undefined;}),dispose:vi.fn(),busy:false,isDisabled:false};
      const renderer={update:vi.fn(),render:vi.fn(),clear:vi.fn(),dispose:vi.fn(),triangleCount:0};
      internals.liveMeshClient=client;internals.liveMeshRenderer=renderer;
      vi.spyOn(internals as unknown as {livePerformancePressure:()=>string},'livePerformancePressure').mockReturnValue(pressure);
      runtime.setLiveMeshMode(mode);
      return{session,runtime,internals,client,renderer};
    }
    const frame=async(session:FakeXrSession,time:number)=>{session.driveNextFrame(time,fakeFrame(session,()=>emptyPose()));await flushMicrotasks();};

    it("'off' neither builds nor draws, whatever the pressure",async()=>{
      const{session,client,renderer}=await meshRuntime('off','low');
      await frame(session,16);await frame(session,32);
      expect(client.request).not.toHaveBeenCalled();expect(renderer.render).not.toHaveBeenCalled();
    });

    it("'auto' follows the phone: the busier it is the less often the mesh is built, and it is never switched off by pressure alone (phone run 2026-11-22: pressure was 'high' for the whole scan, so a rule 'only when low' meant never)",async()=>{
      const quiet=await meshRuntime('auto','low');
      await frame(quiet.session,16);
      expect(quiet.client.request).toHaveBeenCalledTimes(1);expect(quiet.renderer.render).toHaveBeenCalledTimes(1);
      const interval=(rig:{client:{request:{mock:{calls:unknown[][]}}}})=>rig.client.request.mock.calls[0][1] as number;
      const base=interval(quiet);
      expect(base).toBe(LIVE_MESH_MIN_INTERVAL_MS);
      const medium=await meshRuntime('auto','medium');
      await frame(medium.session,16);
      expect(interval(medium)).toBe(base*2);
      const busy=await meshRuntime('auto','high');
      await frame(busy.session,16);
      expect(busy.client.request).toHaveBeenCalledTimes(1);expect(busy.renderer.render).toHaveBeenCalledTimes(1);
      expect(interval(busy)).toBe(base*3.5);
    });

    it("'on' builds at the normal pace under high pressure too, because the operator asked for it",async()=>{
      const{session,client,renderer}=await meshRuntime('on','high');
      await frame(session,16);
      expect(client.request).toHaveBeenCalledTimes(1);expect(renderer.render).toHaveBeenCalledTimes(1);
      expect(client.request.mock.calls[0][1]).toBe(LIVE_MESH_MIN_INTERVAL_MS);
    });

    it('uploads a finished mesh to the renderer once, and draws it every frame',async()=>{
      const{session,internals,renderer}=await meshRuntime('on');
      const triangles=new Float32Array(18);
      internals.liveMeshPending={trianglesXr:triangles,triangleCount:2};
      await frame(session,16);await frame(session,32);
      expect(renderer.update).toHaveBeenCalledTimes(1);expect(renderer.update).toHaveBeenCalledWith(triangles,2);
      expect(renderer.render).toHaveBeenCalledTimes(2);
    });

    it('switching to Off removes the mesh from the camera at once',async()=>{
      const{session,runtime,internals,renderer}=await meshRuntime('on');
      internals.liveMeshPending={trianglesXr:new Float32Array(9),triangleCount:1};
      runtime.setLiveMeshMode('off');
      expect(renderer.clear).toHaveBeenCalled();expect(internals.liveMeshPending).toBeNull();
      await frame(session,16);
      expect(renderer.render).not.toHaveBeenCalled();
    });

    it('draws nothing while the reference space has been reset: the mesh would be in the old frame',async()=>{
      const{session,runtime,internals,renderer}=await meshRuntime('on');
      internals.liveMeshPending={trianglesXr:new Float32Array(9),triangleCount:1};
      (runtime as unknown as {originResetPaused:boolean}).originResetPaused=true;
      renderer.triangleCount=1;
      await frame(session,16);
      expect(renderer.render).not.toHaveBeenCalled();
      expect(renderer.clear).toHaveBeenCalled();
    });

    it('a draw fault drops the live mesh for the session but keeps depth capture and the frame loop alive',async()=>{
      const{session,runtime,internals,client,renderer}=await meshRuntime('on');
      renderer.render.mockImplementation(()=>{throw new Error('mesh draw failed');});
      const captureDepth=vi.spyOn(runtime as unknown as {captureDepth:(time:number,frame:unknown,pose:unknown)=>void},'captureDepth');
      await frame(session,16);
      expect(internals.liveMeshOff).toBe(true);expect(renderer.dispose).toHaveBeenCalled();expect(client.dispose).toHaveBeenCalled();
      expect(runtime.getState().phase).toBe('running');expect(session.endCalls).toBe(0);
      await frame(session,32);
      expect(captureDepth).toHaveBeenCalledTimes(2);
    });

    it('releases the worker and the GPU buffers when the runtime is disposed',async()=>{
      const{runtime,client,renderer}=await meshRuntime('on');
      await runtime.dispose();
      expect(client.dispose).toHaveBeenCalledTimes(1);expect(renderer.dispose).toHaveBeenCalledTimes(1);
    });

  describe('the saved scan mesh over the camera (Continue)',()=>{
    interface RefInternals{referenceMeshRenderer:unknown;referenceMesh:unknown;originResetPaused:boolean}
    async function refRuntime(){
      const session=new FakeXrSession();xr.queue(session);const runtime=createRuntime();await runtime.start();
      const internals=runtime as unknown as RefInternals;
      const renderer={update:vi.fn(),render:vi.fn(),clear:vi.fn(),dispose:vi.fn(),triangleCount:0};
      internals.referenceMeshRenderer=renderer;
      return{session,runtime,internals,renderer};
    }
    const frame=async(session:FakeXrSession,time:number)=>{session.driveNextFrame(time,fakeFrame(session,()=>emptyPose()));await flushMicrotasks();};
    const mesh={trianglesXr:new Float32Array(18),triangleCount:2};

    it('draws nothing until a mesh is set, uploads it once and draws it every frame',async()=>{
      const{session,runtime,renderer}=await refRuntime();
      await frame(session,16);
      expect(renderer.render).not.toHaveBeenCalled();
      runtime.setReferenceMesh(mesh);
      await frame(session,32);await frame(session,48);
      expect(renderer.update).toHaveBeenCalledTimes(1);expect(renderer.update).toHaveBeenCalledWith(mesh.trianglesXr,2);
      expect(renderer.render).toHaveBeenCalledTimes(2);
    });

    it('uploads again when a refined lock sets a new mesh',async()=>{
      const{session,runtime,renderer}=await refRuntime();
      runtime.setReferenceMesh(mesh);await frame(session,16);
      const moved={trianglesXr:new Float32Array(18).fill(1),triangleCount:2};
      runtime.setReferenceMesh(moved);await frame(session,32);
      expect(renderer.update).toHaveBeenCalledTimes(2);expect(renderer.update).toHaveBeenLastCalledWith(moved.trianglesXr,2);
    });

    it('removing it clears the renderer, and nothing is drawn while the reference space has been reset',async()=>{
      const{session,runtime,internals,renderer}=await refRuntime();
      runtime.setReferenceMesh(mesh);await frame(session,16);
      runtime.setReferenceMesh(null);
      expect(renderer.clear).toHaveBeenCalled();
      await frame(session,32);
      expect(renderer.render).toHaveBeenCalledTimes(1);
      runtime.setReferenceMesh(mesh);
      internals.originResetPaused=true;
      await frame(session,48);
      expect(renderer.render).toHaveBeenCalledTimes(1);                                           /* it would be in the old frame */
    });

    it('a draw fault drops only the reference mesh: the frame loop and depth capture go on',async()=>{
      const{session,runtime,internals,renderer}=await refRuntime();
      renderer.render.mockImplementation(()=>{throw new Error('reference mesh draw failed');});
      const captureDepth=vi.spyOn(runtime as unknown as {captureDepth:(time:number,frame:unknown,pose:unknown)=>void},'captureDepth');
      runtime.setReferenceMesh(mesh);
      await frame(session,16);
      expect(internals.referenceMesh).toBeNull();expect(renderer.dispose).toHaveBeenCalled();
      expect(runtime.getState().phase).toBe('running');expect(session.endCalls).toBe(0);
      await frame(session,32);
      expect(captureDepth).toHaveBeenCalledTimes(2);
    });

    it('releases its GPU buffers when the runtime is disposed',async()=>{
      const{runtime,renderer}=await refRuntime();
      runtime.setReferenceMesh(mesh);
      await runtime.dispose();
      expect(renderer.dispose).toHaveBeenCalledTimes(1);
    });
  });
  });

  it('treats context loss as fatal and ignores restoration after teardown',async()=>{
    const session=new FakeXrSession();xr.queue(session);const runtime=createRuntime();await runtime.start();
    const lost=new Event('webglcontextlost',{cancelable:true});browserCanvas.dispatchEvent(lost);
    await until(()=>session.endCalls===1);await flushMicrotasks();
    expect(lost.defaultPrevented).toBe(true);expect(session.endCalls).toBe(1);expect(runtime.getState().phase).toBe('error');
    browserCanvas.dispatchEvent(new Event('webglcontextrestored'));await flushMicrotasks();
    expect(runtime.getState().phase).toBe('error');expect(session.rafRequests).toBe(1);
  });

  it('ends a session that resolves after dispose without starting its frame loop',async()=>{
    const pending=xr.queueDeferred();const runtime=createRuntime();const phases:string[]=[];runtime.subscribe((state)=>phases.push(state.phase));
    const start=runtime.start();await until(()=>xr.requests===1);await runtime.dispose();
    const session=new FakeXrSession();pending.resolve(session);await start;
    expect(session.endCalls).toBe(1);expect(session.liveFrameCallbacks).toBe(0);expect(wakeLock.requests).toBe(0);
    expect(phases).not.toContain('running');expect(document.documentElement.classList.contains('xr-session-active')).toBe(false);
  });

  it.each(['makeXRCompatible','updateRenderState','requestReferenceSpace','requestReferenceSpaceFallback','requestLightProbe'])('stops the stale start after %s resolves',async(boundary)=>{
    const held=deferred<void>();let localFloorFailure:ReturnType<typeof deferred<object>>|null=null;
    if(boundary==='makeXRCompatible')gl.makeXRCompatible=()=>held.promise;
    const session=new FakeXrSession();xr.queue(session);
    if(boundary==='updateRenderState')session.updateRenderStatePromise=held.promise;
    if(boundary==='requestReferenceSpace')session.referenceSpacePromises.set('local-floor',held.promise.then(()=>({})));
    if(boundary==='requestReferenceSpaceFallback'){
      localFloorFailure=deferred<object>();
      session.referenceSpacePromises.set('local-floor',localFloorFailure.promise);
      session.referenceSpacePromises.set('local',held.promise.then(()=>({})));
    }
    if(boundary==='requestLightProbe')session.lightProbePromise=held.promise.then(()=>({}));
    const runtime=createRuntime();const start=runtime.start();
    const reached=():boolean=>boundary==='makeXRCompatible'?session.listenerCount('end')===1:
      boundary==='updateRenderState'?session.updateCalls===1:
      boundary==='requestReferenceSpace'?session.referenceSpaceRequests.length===1:
      boundary==='requestReferenceSpaceFallback'?session.referenceSpaceRequests.length===2:session.lightProbeRequests===1;
    if(boundary==='requestReferenceSpaceFallback'){
      await until(()=>session.referenceSpaceRequests.length===1);
      localFloorFailure!.reject(new Error('local-floor unavailable'));
    }
    await until(reached);await runtime.dispose();held.resolve();await start;await flushMicrotasks();
    expect(session.endCalls).toBe(1);expect(session.liveFrameCallbacks).toBe(0);expect(wakeLock.requests).toBe(0);
    expect(document.documentElement.classList.contains('xr-session-active')).toBe(false);
    expect(glContextsCreated).toBe(glContextsLost);
    if(boundary==='updateRenderState')expect(session.referenceSpaceRequests).toHaveLength(0);
    if(boundary==='requestReferenceSpace')expect(session.referenceSpaceRequests).toEqual(['local-floor']);
    if(boundary==='requestReferenceSpaceFallback')expect(session.referenceSpaceRequests).toEqual(['local-floor','local']);
    if(boundary==='requestLightProbe')expect(session.rafRequests).toBe(0);
  });

  it('returns session, listener, frame, GL and wake-lock counters to baseline across 50 delayed enter/exit cycles',async()=>{
    const addListener=vi.spyOn(document,'addEventListener');const removeListener=vi.spyOn(document,'removeEventListener');
    for(let index=0;index<50;index+=1){
      const pending=xr.queueDeferred();const runtime=createRuntime();const start=runtime.start();await until(()=>xr.requests===index+1);
      const session=new FakeXrSession();await runtime.dispose();pending.resolve(session);await start;
      expect(session.endCalls).toBe(1);expect(session.listenerCount('end')).toBe(0);expect(session.listenerCount('visibilitychange')).toBe(0);
      expect(session.liveFrameCallbacks).toBe(0);expect(wakeLock.liveSentinels.size).toBe(0);expect(document.documentElement.classList.contains('xr-session-active')).toBe(false);
    }
    const additions=addListener.mock.calls.filter(([type])=>type==='visibilitychange').length;const removals=removeListener.mock.calls.filter(([type])=>type==='visibilitychange').length;
    expect(xr.liveSessions).toBe(0);expect(glContextsCreated).toBe(glContextsLost);expect(additions).toBe(removals);
  });

  it('releases a wake lock granted after the session has ended and never publishes it active',async()=>{
    const pending=wakeLock.deferNext();const session=new FakeXrSession();xr.queue(session);const runtime=createRuntime();const wakeStates:boolean[]=[];
    runtime.subscribe((state)=>wakeStates.push(state.wakeLockActive));await runtime.start();await until(()=>wakeLock.requests===1);
    await runtime.stop();const sentinel=wakeLock.createSentinel();pending.resolve(sentinel);await flushMicrotasks();
    expect(sentinel.releaseCalls).toBe(1);expect(runtime.getState().wakeLockActive).toBe(false);expect(wakeStates).not.toContain(true);expect(wakeLock.liveSentinels.size).toBe(0);
  });

  it('releases and reacquires across visibility changes, then releases on session end',async()=>{
    const session=new FakeXrSession();xr.queue(session);const runtime=createRuntime();await runtime.start();await flushMicrotasks();
    expect(wakeLock.liveSentinels.size).toBe(1);
    setVisibility('hidden');await flushMicrotasks();
    expect(wakeLock.liveSentinels.size).toBe(0);
    setVisibility('visible');await flushMicrotasks();
    expect(wakeLock.liveSentinels.size).toBe(1);await runtime.stop();
    expect(wakeLock.liveSentinels.size).toBe(0);
  });

  it('keeps Finish failure recoverable and retries to a fully ended session',async()=>{
    const session=new FakeXrSession();session.depthUsage='cpu-optimized';session.depthDataFormat='luminance-alpha';session.depthType='raw';session.depthActive=true;session.endFailures=1;xr.queue(session);const runtime=createRuntime();await runtime.start();
    await runtime.stop();expect(runtime.getState().phase).toBe('running');expect(runtime.getState().error).toContain('Fake XR session end failed');
    expect(session.active).toBe(true);expect(session.liveFrameCallbacks).toBe(1);
    const frozen=runtime.getPointCloudSnapshot();session.driveNextFrame(16,evidenceFrame(session));
    expect(runtime.getPointCloudSnapshot()).toBe(frozen);expect(runtime.getState().depthFrameCount).toBe(frozen.depthFrameCount);
    await runtime.stop();expect(runtime.getState().phase).toBe('ended');expect(session.active).toBe(false);
    expect(session.liveFrameCallbacks).toBe(0);expect(wakeLock.liveSentinels.size).toBe(0);expect(session.endCalls).toBe(2);
  });

  it('freezes the evidence epoch at Finish while RAF rendering continues until XR end',async()=>{
    const info=vi.spyOn(clientDiagnostics,'info');
    const run=async(endFrames:number)=>{
      const session=new FakeXrSession();session.depthUsage='cpu-optimized';session.depthDataFormat='luminance-alpha';session.depthType='raw';session.depthActive=true;
      const ending=session.deferEnd();xr.queue(session);const runtime=createRuntime();await runtime.start();
      const internal=runtime as unknown as {
        keyframeSelector:{evaluate:(time:number,pose:unknown)=>unknown};
        pointCloud:{sampleDepthFrame:(...args:any[])=>unknown};
        captureNativePlaneGuidance:(...args:any[])=>void;captureRgbGuidance:(...args:any[])=>void;
        captureTagGuidance:(time:number,view:unknown)=>void;maybeRecordVisualEvidence:(...args:any[])=>void;
        acceptTagFrame:(frame:TagCameraFrame|null)=>void;
      };
      let keyframeId=0;
      internal.keyframeSelector.evaluate=()=>({kind:'keyframe',state:'keyframe',frameWeight:1,keyframeId:++keyframeId,translationFromKeyframeMeters:0,rotationFromKeyframeRadians:0,linearSpeedMetersPerSecond:0,angularSpeedRadiansPerSecond:0,linearAccelerationMetersPerSecond2:0,angularAccelerationRadiansPerSecond2:0,motionQuality:1});
      const admitted={native:0,rgb:0,tag:0,visual:0};
      internal.captureNativePlaneGuidance=()=>{admitted.native+=1;};
      internal.captureRgbGuidance=()=>{admitted.rgb+=1;};
      internal.captureTagGuidance=(time)=>{admitted.tag+=1;internal.acceptTagFrame(fakeTagFrame(time));};
      internal.maybeRecordVisualEvidence=()=>{admitted.visual+=1;};
      const sampleDepth=vi.spyOn(internal.pointCloud,'sampleDepthFrame');
      const tagFrames:TagCameraFrame[]=[];runtime.subscribeTagFrames((frame)=>tagFrames.push(frame));
      for(const time of [16,200,400,600])session.driveNextFrame(time,evidenceFrame(session));
      expect(runtime.getState().depthFrameCount).toBe(4);expect(runtime.getState().sampledDepthFrames).toBe(4);
      expect(sampleDepth).toHaveBeenCalledTimes(4);expect(admitted.native).toBe(4);expect(admitted.rgb).toBe(4);
      expect(admitted.tag).toBe(4);expect(admitted.visual).toBe(4);expect(tagFrames).toHaveLength(4);

      const stop=runtime.stop();await until(()=>session.endCalls===1);expect(runtime.getState().phase).toBe('stopping');
      const frozen=runtime.getPointCloudSnapshot();const evidenceBefore={...admitted};const sampledBefore=sampleDepth.mock.calls.length;
      for(let index=0;index<endFrames;index+=1)session.driveNextFrame(800+index*16,evidenceFrame(session));
      internal.acceptTagFrame(fakeTagFrame(5000));
      expect(runtime.getPointCloudSnapshot()).toBe(frozen);
      expect(runtime.getState().depthFrameCount).toBe(frozen.depthFrameCount);expect(runtime.getState().sampledDepthFrames).toBe(frozen.sampledDepthFrames);
      expect(sampleDepth).toHaveBeenCalledTimes(sampledBefore);expect(admitted).toEqual(evidenceBefore);expect(tagFrames).toHaveLength(4);
      await runtime.stop();expect(session.endCalls).toBe(1);
      ending.resolve(undefined);await stop;
      expect(runtime.getState().phase).toBe('ended');expect(runtime.getPointCloudSnapshot()).toBe(frozen);
      const data=frozen as unknown as Record<string,unknown>;
      const encoded=JSON.stringify(Object.fromEntries(Object.entries(data).map(([key,value])=>ArrayBuffer.isView(value)
        ?[key,Array.from(new Uint8Array(value.buffer,value.byteOffset,value.byteLength))]:[key,value])));
      await runtime.dispose();
      return{encoded,frozen};
    };

    const short=await run(6);const long=await run(240);
    expect(short.encoded).toBe(long.encoded);
    const finishRequests=info.mock.calls.filter(([message])=>message==='XR_FINISH_REQUEST').map(([,details])=>details as Record<string,unknown>);
    const finished=info.mock.calls.filter(([message])=>message==='XR_SCAN_FINISHED').map(([,details])=>details as Record<string,unknown>);
    expect(finishRequests).toHaveLength(2);expect(finished).toHaveLength(2);
    for(let index=0;index<2;index+=1){
      const snapshot=index===0?short.frozen:long.frozen;
      expect(finishRequests[index]).toMatchObject({evidenceEpoch:1,depthFrames:snapshot.depthFrameCount,integratedFrames:snapshot.sampledDepthFrames,stablePoints:snapshot.pointCount,retainedPoints:snapshot.rawPointCount,keyframes:snapshot.integrationKeyframes});
      expect(finished[index]).toMatchObject({evidenceEpoch:1,depthFrames:snapshot.depthFrameCount,integratedFrames:snapshot.sampledDepthFrames,stablePoints:snapshot.pointCount,retainedPoints:snapshot.rawPointCount,keyframes:snapshot.integrationKeyframes});
    }
  });

  it('finalizes one cached snapshot for Finish and uses cheap counters for interruption',async()=>{
    const snapshot=vi.spyOn(DepthPointCloudAccumulator.prototype,'snapshot');
    const finishedSession=new FakeXrSession();xr.queue(finishedSession);const finished=createRuntime();await finished.start();await finished.stop();
    const finalSnapshot=finished.getPointCloudSnapshot();
    expect(snapshot).toHaveBeenCalledTimes(1);expect(finished.getPointCloudSnapshot()).toBe(finalSnapshot);
    expect(finished.getState().phase).toBe('ended');expect(finished.getState().pointCount).toBe(finalSnapshot.pointCount);

    snapshot.mockClear();const interruptedSession=new FakeXrSession();xr.queue(interruptedSession);const interrupted=createRuntime();await interrupted.start();interruptedSession.interrupt();
    await flushMicrotasks();expect(snapshot).not.toHaveBeenCalled();expect(interrupted.getState().phase).toBe('interrupted');
    expect(interrupted.getState().pointCount).toBe(0);expect(interrupted.getState().retainedPointCount).toBe(0);
  });

  it('clears the cached final snapshot when a new scan generation starts',async()=>{
    const snapshot=vi.spyOn(DepthPointCloudAccumulator.prototype,'snapshot');const firstSession=new FakeXrSession();xr.queue(firstSession);
    const runtime=createRuntime();await runtime.start();await runtime.stop();const first=runtime.getPointCloudSnapshot();
    const nextSession=new FakeXrSession();xr.queue(nextSession);await runtime.start();const activeGeneration=runtime.getPointCloudSnapshot();
    expect(activeGeneration).not.toBe(first);expect(snapshot).toHaveBeenCalledTimes(2);
    await runtime.stop();const second=runtime.getPointCloudSnapshot();expect(second).not.toBe(first);expect(runtime.getPointCloudSnapshot()).toBe(second);
  });
});

describe('XRReferenceSpace reset inside a running session (issue #36)',()=>{
  class FakeReferenceSpace extends EventTarget{
    added=0;removed=0;
    addEventListener(type:string,listener:EventListenerOrEventListenerObject|null,options?:boolean|AddEventListenerOptions):void{if(type==='reset')this.added+=1;super.addEventListener(type,listener,options);}
    removeEventListener(type:string,listener:EventListenerOrEventListenerObject|null,options?:boolean|EventListenerOptions):void{if(type==='reset')this.removed+=1;super.removeEventListener(type,listener,options);}
  }
  const depthSession=(space:FakeReferenceSpace):FakeXrSession=>{
    const session=new FakeXrSession();session.depthUsage='cpu-optimized';session.depthDataFormat='luminance-alpha';session.depthType='raw';session.depthActive=true;
    session.referenceSpacePromises.set('local-floor',Promise.resolve(space));return session;
  };

  it('pauses evidence collection on reset, warns, and never mixes the two coordinate frames',async()=>{
    const warn=vi.spyOn(clientDiagnostics,'warn');
    const space=new FakeReferenceSpace();const session=depthSession(space);xr.queue(session);
    const runtime=createRuntime();await runtime.start();await flushMicrotasks();
    expect(space.added).toBe(1);
    expect(runtime.getState().originReset).toBe(false);

    session.driveNextFrame(16,evidenceFrame(session));
    expect(runtime.getState().depthFrameCount).toBe(1);

    const event=Object.assign(new Event('reset'),{transform:{matrix:new Float32Array(16)}});
    space.dispatchEvent(event);
    expect(runtime.getState().originReset).toBe(true);
    expect(runtime.getState().scanPaused).toBe(true);
    expect(warn).toHaveBeenCalledWith('XR_REFERENCE_SPACE_RESET',expect.objectContaining({transformAvailable:true,action:'evidence_paused'}));

    for(const time of [200,400,600])session.driveNextFrame(time,evidenceFrame(session));
    expect(runtime.getState().depthFrameCount).toBe(1);               /* no depth sample after the reset */
    expect(runtime.getState().originReset).toBe(true);                /* the periodic state update keeps the pause visible */
    expect(runtime.getState().scanPaused).toBe(true);
    expect(runtime.getState().phase).toBe('running');                 /* Finish still works: what was scanned before the reset is kept */
  });

  it('reports a reset without a transform the same way and reacts only once',async()=>{
    const warn=vi.spyOn(clientDiagnostics,'warn');
    const space=new FakeReferenceSpace();const session=depthSession(space);xr.queue(session);
    const runtime=createRuntime();await runtime.start();await flushMicrotasks();
    space.dispatchEvent(new Event('reset'));space.dispatchEvent(new Event('reset'));
    expect(runtime.getState().originReset).toBe(true);
    expect(warn.mock.calls.filter(([event])=>event==='XR_REFERENCE_SPACE_RESET')).toHaveLength(1);
    expect(warn).toHaveBeenCalledWith('XR_REFERENCE_SPACE_RESET',expect.objectContaining({transformAvailable:false}));
  });

  it('removes the listener on stop and ignores a reset that arrives afterwards',async()=>{
    const space=new FakeReferenceSpace();const session=depthSession(space);xr.queue(session);
    const runtime=createRuntime();await runtime.start();await flushMicrotasks();
    await runtime.stop();
    expect(space.removed).toBe(space.added);
    space.dispatchEvent(new Event('reset'));
    expect(runtime.getState().originReset).toBe(false);
  });

  it('starts the next scan without the pause',async()=>{
    const space=new FakeReferenceSpace();const session=depthSession(space);xr.queue(session);
    const runtime=createRuntime();await runtime.start();await flushMicrotasks();
    space.dispatchEvent(new Event('reset'));expect(runtime.getState().originReset).toBe(true);
    await runtime.stop();
    const nextSpace=new FakeReferenceSpace();const next=depthSession(nextSpace);xr.queue(next);
    await runtime.start();await flushMicrotasks();
    expect(runtime.getState().originReset).toBe(false);
    next.driveNextFrame(16,evidenceFrame(next));
    expect(runtime.getState().depthFrameCount).toBe(1);
  });
});
