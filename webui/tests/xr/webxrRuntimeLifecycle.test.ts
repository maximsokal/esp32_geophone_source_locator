import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { DepthPointCloudAccumulator } from '../../src/xr/depthPointCloud';
import { WebXrRuntime } from '../../src/xr/webxrRuntime';
import { deferred, FakeWakeLock, FakeXrLayer, FakeXrSession, FakeXrSystem } from './support/xrFakes';

const runtimes:WebXrRuntime[]=[];
let xr:FakeXrSystem;
let wakeLock:FakeWakeLock;
let gl:{getExtension:()=>{loseContext:()=>void}|null;makeXRCompatible?:()=>Promise<void>};
let glContextsCreated=0;let glContextsLost=0;
type StubListener=EventListenerOrEventListenerObject;
interface StubDocument{
  visibilityState:string;
  body:object;
  documentElement:{classList:{add:(name:string)=>void;remove:(name:string)=>void;contains:(name:string)=>boolean}};
  addEventListener:(type:string,listener:StubListener|null)=>void;
  removeEventListener:(type:string,listener:StubListener|null)=>void;
  dispatchEvent:(event:Event)=>boolean;
  createElement:(name:string)=>{getContext:(kind:string,attributes?:unknown)=>unknown|null};
}
let browserDocument:StubDocument;

function dispatchDocument(type:string):void{
  browserDocument.dispatchEvent({type} as Event);
}

function setVisibility(value:string):void{
  browserDocument.visibilityState=value;
  dispatchDocument('visibilitychange');
}

function installEnvironment(makeCompatible?:()=>Promise<void>):void{
  xr=new FakeXrSystem();wakeLock=new FakeWakeLock();glContextsCreated=0;glContextsLost=0;gl={getExtension:()=>({loseContext:()=>{glContextsLost+=1;}}),makeXRCompatible:makeCompatible??(()=>Promise.resolve())};
  const listeners=new Map<string,Set<StubListener>>();
  const classes=new Set<string>();
  browserDocument={
    visibilityState:'visible',
    body:{},
    documentElement:{classList:{
      add:(name)=>{classes.add(name);},
      remove:(name)=>{classes.delete(name);},
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
    createElement:()=>({
      getContext:(kind)=>{
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

function createRuntime():WebXrRuntime{const runtime=new WebXrRuntime();runtimes.push(runtime);return runtime;}
async function flushMicrotasks():Promise<void>{for(let i=0;i<8;i+=1)await Promise.resolve();}
async function until(check:()=>boolean):Promise<void>{for(let i=0;i<20&&!check();i+=1)await Promise.resolve();if(!check())throw new Error('Expected lifecycle operation did not reach its deferred boundary.');}

beforeEach(()=>installEnvironment());
afterEach(async()=>{
  await Promise.all(runtimes.splice(0).map((runtime)=>runtime.dispose()));
  vi.restoreAllMocks();vi.unstubAllGlobals();
});

describe('WebXrRuntime lifecycle ownership',()=>{
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
    const session=new FakeXrSession();session.endFailures=1;xr.queue(session);const runtime=createRuntime();await runtime.start();
    await runtime.stop();expect(runtime.getState().phase).toBe('running');expect(runtime.getState().error).toContain('Fake XR session end failed');
    expect(session.active).toBe(true);expect(session.liveFrameCallbacks).toBe(1);
    await runtime.stop();expect(runtime.getState().phase).toBe('ended');expect(session.active).toBe(false);
    expect(session.liveFrameCallbacks).toBe(0);expect(wakeLock.liveSentinels.size).toBe(0);expect(session.endCalls).toBe(2);
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
