import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createElement, StrictMode, type ComponentProps } from 'react';
import { act, create, type ReactTestInstance, type ReactTestRenderer } from 'react-test-renderer';

const panelHarness=vi.hoisted(()=>({
  runtimes:[] as any[],workers:[] as any[],
  fetchLocalizationRuns:vi.fn(),loadSessionRegistration:vi.fn(),saveSessionRegistration:vi.fn(),probeCapabilities:vi.fn(),
  makeState:(()=>({})) as (()=>any)|null,
}));

vi.mock('../../src/api/deviceApi',async(importOriginal)=>{
  const original=await importOriginal<typeof import('../../src/api/deviceApi')>();
  return{...original,fetchLocalizationRuns:panelHarness.fetchLocalizationRuns,loadSessionRegistration:panelHarness.loadSessionRegistration,saveSessionRegistration:panelHarness.saveSessionRegistration};
});
vi.mock('../../src/xr/webxrRuntime',()=>({
  WebXrRuntime:class{
    state:any;generation=0;listeners=new Set<(state:any)=>void>();tagListeners=new Set<(frame:any)=>void>();tagUnsubscribes=0;tagCaptureDisables=0;attachCalls=0;attached=false;detachCalls=0;disposed=0;
    finalSnapshot={identity:Symbol('scan snapshot')};
    constructor(){this.state=panelHarness.makeState?.()??{phase:'idle'};panelHarness.runtimes.push(this);}
    attach=()=>{this.attachCalls+=1;this.attached=true;};detach=async()=>{this.detachCalls+=1;this.attached=false;};getState=()=>this.state;getLifecycleGeneration=()=>this.generation;
    subscribe=(listener:(state:any)=>void)=>{this.listeners.add(listener);listener(this.state);return()=>this.listeners.delete(listener);};
    subscribeTagFrames=(listener:(frame:any)=>void)=>{this.tagListeners.add(listener);return()=>{this.tagListeners.delete(listener);this.tagUnsubscribes+=1;};};
    emitTagFrame=(frame:any)=>{for(const listener of this.tagListeners)listener(frame);};
    setTagCaptureEnabled=(enabled:boolean)=>{if(!enabled)this.tagCaptureDisables+=1;return null;};
    getPointCloudSnapshot=()=>this.finalSnapshot;getLastManualPointId=()=>null;getStructuralGuideSnapshot=()=>({positions:new Float32Array(),normals:new Float32Array(),confidence:new Float32Array(),pointCount:0,keyframeCount:0,voxelSizeMeters:0});
    projectProjectPointToNormalized=()=>null;addSuggestedManualPoint=()=>null;queueManualPointAtNormalized=()=>undefined;undoLastManualPoint=()=>undefined;
    private publish(phase:string,patch:Record<string,unknown>={}){this.state={...this.state,...patch,phase};for(const listener of this.listeners)listener(this.state);}
    setPhase=(phase:string,patch:Record<string,unknown>={})=>{if(['ended','interrupted','error'].includes(phase)&&this.state.phase!==phase)this.generation+=1;this.publish(phase,patch);};
    start=async()=>{this.generation+=1;this.publish('starting');};
    stop=async()=>{this.publish('stopping');this.generation+=1;this.publish('ended',{pointCount:1});};
    dispose=async()=>{this.disposed+=1;};
  },
  probeWebXrCapabilities:()=>panelHarness.probeCapabilities(),
}));
vi.mock('../../src/xr/xrLaunchPolicy',()=>({
  canAttemptImmersiveAr:()=>true,
  launchImmersiveArFromUserGesture:async(options:any)=>options.startRuntime(),
}));
vi.mock('../../src/xr/optionalMonocularPrior',()=>({optionalMonocularPrior:{status:'disabled',error:null,calibrationConfidence:0,calibrationModel:null,hasStagedPostScanFrame:false,isBusy:false,subscribe:()=>()=>undefined,setEnabled:()=>undefined}}));
vi.mock('../../src/components/StructuralGuideOverlay',()=>({StructuralGuideOverlay:()=>null}));
vi.mock('../../src/xr/fiducial/tagDetector.worker.ts?worker&inline',()=>({default:class{
  messages:any[]=[];terminated=0;private listeners=new Map<string,Set<(event:any)=>void>>();
  constructor(){panelHarness.workers.push(this);}
  addEventListener(type:string,listener:(event:any)=>void){const bucket=this.listeners.get(type)??new Set();bucket.add(listener);this.listeners.set(type,bucket);}
  removeEventListener(type:string,listener:(event:any)=>void){this.listeners.get(type)?.delete(listener);}
  postMessage(message:any){this.messages.push(message);}
  terminate(){this.terminated+=1;}
  emit(type:string,data:unknown){for(const listener of this.listeners.get(type)??[])listener({data,message:'fake worker error'});}
}}));
vi.mock('../../src/xr/fiducial/tagPose',async(importOriginal)=>{
  const original=await importOriginal<typeof import('../../src/xr/fiducial/tagPose')>();
  return{...original,estimateTagPose:(input:any)=>({ok:true,positionScan:[Number(input.corners[0][0]),2,3],capturedAtMs:input.capturedAtMs,cameraPosition:input.cameraPosition,cameraForward:input.cameraForward})};
});
vi.mock('../../src/xr/fiducial/registrationArtifact',async(importOriginal)=>{
  const original=await importOriginal<typeof import('../../src/xr/fiducial/registrationArtifact')>();
  return{...original,decodeRegistrationArtifact:(json:string)=>JSON.parse(json)};
});

import { XrRuntimePanel } from '../../src/components/XrRuntimePanel';
import { LocaleProvider, translate } from '../../src/i18n';
import type { LocalizationSessionGeometry } from '../../src/api/deviceApi';
import { TagObservationStore } from '../../src/xr/fiducial/tagObservationStore';
import * as tagObservationFusion from '../../src/xr/fiducial/tagObservationFusion';
import { deferred } from '../xr/support/xrFakes';

const mounted:ReactTestRenderer[]=[];
type BrowserListener=EventListenerOrEventListenerObject;
interface PanelWindowStub{
  isSecureContext:boolean;
  innerWidth:number;
  innerHeight:number;
  devicePixelRatio:number;
  localStorage:{getItem:(key:string)=>string|null;setItem:(key:string,value:string)=>void};
  setTimeout:(callback:()=>void,delay?:number)=>number;
  clearTimeout:(id:number)=>void;
  setInterval:(callback:()=>void,delay?:number)=>number;
  clearInterval:(id:number)=>void;
  addEventListener:(type:string,listener:BrowserListener|null)=>void;
  removeEventListener:(type:string,listener:BrowserListener|null)=>void;
}
interface PanelDocumentStub{
  hidden:boolean;
  visibilityState:string;
  body:object;
  documentElement:{classList:{add:(name:string)=>void;remove:(name:string)=>void;contains:(name:string)=>boolean}};
  addEventListener:(type:string,listener:BrowserListener|null)=>void;
  removeEventListener:(type:string,listener:BrowserListener|null)=>void;
}
let browserWindow:PanelWindowStub;
let browserDocument:PanelDocumentStub;
const validCapabilities={secureContext:true,cameraApi:true,rawXrRgbApi:true,tagReadbackMode:'nonblocking',webGl:true,xrApi:true,immersiveAr:true,wakeLockApi:true,videoInputEnumeration:false,videoInputsEnumerated:false,videoInputCount:null,videoInputLabelsExposed:false,hardwareDepthSensor:'unknown',workerApi:true,sharedMemory:false,wasmThreads:false,webGpu:false};

function runtimeState(phase='idle',patch:Record<string,unknown>={}){
  return{phase,referenceSpace:null,domOverlay:false,environmentBlendMode:null,frameCount:0,tracking:false,pose:null,scanPaused:false,wakeLockActive:false,wakeLockError:null,depthAvailable:false,depthUsage:null,depthDataFormat:null,depthType:null,depthWidth:null,depthHeight:null,depthFrameCount:0,sampledDepthFrames:0,pointCount:0,retainedPointCount:0,pointCloudVersion:0,keyframeCount:0,polishFrameCount:0,motionRejectedFrames:0,motionState:null,motionQuality:0,linearSpeedMetersPerSecond:0,angularSpeedRadiansPerSecond:0,linearAccelerationMetersPerSecond2:0,coverageGuideActive:false,coverageGuideRenderPoints:0,coverageVisibleCells:0,coverageReadyCells:0,coverageTotalCells:0,coverageTargetIndependentViews:3,coverageAverageConfidence:0,coverageWeakPoints:0,coverageImprovingPoints:0,coverageStablePoints:0,coverageGuideError:null,manualPointCount:0,manualEdgeCount:0,manualPointError:null,performance:{pressure:'low',adaptiveCoverageIntervalMs:550,frameDeltaP95Ms:0,fusionP50Ms:0,fusionP95Ms:0,coverageP95Ms:0,longFrames33Ms:0,longFrames50Ms:0},lightingEstimateAvailable:false,primaryLightIntensity:null,rgbGuidanceAvailable:false,rgbGuidanceWidth:null,rgbGuidanceHeight:null,visualEvidenceFrames:0,monocularPriorStatus:'disabled',depthError:null,error:null,...patch};
}
function geometry():LocalizationSessionGeometry{return{available:true,pods:[{pod:1,computed:true,leveled:true,computed_at_ms:1234}]};}
function listing(){return{geometry:geometry()};}
const BASE_ID='scan-crc32:aaaaaaaa';
const CONTINUE={scanIntent:'continue' as const,baseScanFrameId:BASE_ID};
/** The decoder is mocked as JSON.parse here, so the fixture must already satisfy the applicability predicate. */
function registrationFixture(session:string,patch:Record<string,unknown>={}){
  return{session,schema:1,valid:true,geometry_computed_at_ms:1234,scan_frame_id:BASE_ID,sensor_to_scan:[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],residual_rms_m:0.01,threshold_m:0.05,tier:'minimum',reason:null,...patch};
}
function registrationJson(session:string,patch:Record<string,unknown>={}):string{return JSON.stringify(registrationFixture(session,patch));}
async function settleReact():Promise<void>{for(let i=0;i<6;i+=1)await Promise.resolve();}
async function mountPanel(props:ComponentProps<typeof XrRuntimePanel>={},strict=false){
  let renderer!:ReactTestRenderer;await act(async()=>{const panel=createElement(LocaleProvider,{initialLocale:'en'},createElement(XrRuntimePanel,props));renderer=create(strict?createElement(StrictMode,null,panel):panel);await settleReact();});mounted.push(renderer);return renderer;
}
function unmount(renderer:ReactTestRenderer):void{act(()=>renderer.unmount());const index=mounted.indexOf(renderer);if(index>=0)mounted.splice(index,1);}
function textOf(node:ReactTestInstance|string):string{return typeof node==='string'?node:node.children.map(textOf).join('');}
function tagButton(renderer:ReactTestRenderer):ReactTestInstance{
  const title=translate('en','xr.tag.capture_title');const result=renderer.root.findAllByType('button').find((button)=>button.props.title===title);if(result===undefined)throw new Error('Missing AprilTag button.');return result;
}
function emitTagDetection(worker:any,options:{tagId?:number;positionX?:number}={}):void{
  const request=worker.messages.filter((item:any)=>item.type==='detect').at(-1);if(request===undefined)throw new Error('Expected an in-flight tag detection.');
  const x=options.positionX??1;worker.emit('message',{type:'result',requestId:request.requestId,epoch:request.epoch,detections:[{tagId:options.tagId??0,corners:[[x,0],[x+1,0],[x+1,1],[x,1]],hammingDistance:0}]});
}
function tagFrame(capturedAtMs=1,cameraPosition:readonly[number,number,number]=[0,0,0],cameraForward:readonly[number,number,number]=[0,0,-1]){
  return{width:1,height:1,rgba:new Uint8Array(4),capturedAtMs,cameraPosition,cameraForward,projectionMatrix:new Float32Array(16),worldFromView:new Float32Array(16)};
}
async function startGeometry(runtime:any):Promise<void>{
  await act(async()=>{runtime.setPhase('starting');await settleReact();});
  await act(async()=>{runtime.setPhase('running');await settleReact();});
}

class PanelBluetooth{
  listeners=new Set<EventListenerOrEventListenerObject>();requestCalls=0;scanStops=0;liveScans=0;nextRequest:Promise<{stop():void}>|null=null;
  addEventListener(_type:string,listener:EventListenerOrEventListenerObject|null){if(listener!==null)this.listeners.add(listener);}
  removeEventListener(_type:string,listener:EventListenerOrEventListenerObject|null){if(listener!==null)this.listeners.delete(listener);}
  requestLEScan=async():Promise<{stop():void}>=>{this.requestCalls+=1;if(this.nextRequest!==null)return this.nextRequest;return this.createScan();};
  createScan(){this.liveScans+=1;let stopped=false;return{stop:()=>{if(stopped)return;stopped=true;this.scanStops+=1;this.liveScans-=1;}};}
  advertise(){const event=Object.assign(new Event('advertisementreceived'),{rssi:-60,serviceData:{get:()=>new DataView(new Uint8Array([1]).buffer)}});for(const listener of this.listeners){if(typeof listener==='function')listener.call(this,event);else listener.handleEvent(event);}}
}

beforeEach(()=>{
  panelHarness.runtimes=[];panelHarness.workers=[];panelHarness.makeState=()=>runtimeState();
  panelHarness.fetchLocalizationRuns.mockReset().mockResolvedValue(listing());panelHarness.loadSessionRegistration.mockReset().mockResolvedValue(null);
  panelHarness.saveSessionRegistration.mockReset().mockResolvedValue({ok:true});panelHarness.probeCapabilities.mockReset().mockResolvedValue(validCapabilities);
  let nextTimerId=0;
  const timers=new Map<number,()=>void>();
  const intervals=new Map<number,()=>void>();
  const listeners=new Map<string,Set<BrowserListener>>();
  const classes=new Set<string>();
  const addListener=(type:string,listener:BrowserListener|null):void=>{
    if(listener===null)return;
    const bucket=listeners.get(type)??new Set<BrowserListener>();
    bucket.add(listener);
    listeners.set(type,bucket);
  };
  const removeListener=(type:string,listener:BrowserListener|null):void=>{
    if(listener===null)return;
    listeners.get(type)?.delete(listener);
  };
  browserWindow={
    isSecureContext:false,
    innerWidth:1280,
    innerHeight:720,
    devicePixelRatio:1,
    localStorage:{getItem:()=>null,setItem:()=>undefined},
    setTimeout:(callback)=>{const id=++nextTimerId;timers.set(id,callback);return id;},
    clearTimeout:(id)=>{timers.delete(id);},
    setInterval:(callback)=>{const id=++nextTimerId;intervals.set(id,callback);return id;},
    clearInterval:(id)=>{intervals.delete(id);},
    addEventListener:addListener,
    removeEventListener:removeListener,
  };
  browserDocument={
    hidden:false,
    visibilityState:'visible',
    body:{},
    documentElement:{classList:{
      add:(name)=>{classes.add(name);},
      remove:(name)=>{classes.delete(name);},
      contains:(name)=>classes.has(name),
    }},
    addEventListener:addListener,
    removeEventListener:removeListener,
  };
  vi.stubGlobal('window',browserWindow);
  vi.stubGlobal('document',browserDocument);
  vi.stubGlobal('navigator',{});
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT',true);
});
afterEach(()=>{for(const renderer of [...mounted])unmount(renderer);vi.unstubAllGlobals();vi.clearAllMocks();});

describe('XrRuntimePanel lifecycle',()=>{
  it('attaches only a committed runtime through StrictMode and detaches it on cleanup',async()=>{
    const renderer=await mountPanel({objectId:'OBJ_STRICT',sessionId:'SESSION_STRICT'},true);
    const activated=panelHarness.runtimes.filter((runtime:any)=>runtime.attached);
    expect(activated).toHaveLength(1);expect(activated[0].attachCalls).toBeGreaterThan(0);
    const runtime=activated[0];unmount(renderer);
    expect(runtime.attached).toBe(false);expect(runtime.detachCalls).toBeGreaterThan(0);
  });

  it('refuses tag mode without non-blocking readback and disables Finish while stopping',async()=>{
    panelHarness.probeCapabilities.mockResolvedValue({...validCapabilities,tagReadbackMode:'synchronous'});
    const renderer=await mountPanel({objectId:'OBJ_A',sessionId:'SESSION_A'});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    expect(tagButton(renderer).props.disabled).toBe(true);
    expect(renderer.root.findAll((node)=>node.props.className==='xr-overlay-hint').map(textOf).join(' ')).toContain(translate('en','xr.tag.readback_unavailable'));
    const finish=renderer.root.findAllByType('button').find((button)=>textOf(button)===translate('en','xr.finish_save'));
    if(finish===undefined)throw new Error('Missing Finish button.');expect(finish.props.disabled).toBe(false);
    await act(async()=>{runtime.setPhase('stopping');await settleReact();});expect(finish.props.disabled).toBe(true);
    unmount(renderer);
  });

  it('tears down tag worker and subscription on unmount without saving registration',async()=>{
    const renderer=await mountPanel({objectId:'OBJ_A',sessionId:'SESSION_A'});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    expect(runtime.attached).toBe(true);
    await act(async()=>{tagButton(renderer).props.onClick();await settleReact();});const worker=panelHarness.workers[0];
    expect(worker).toBeDefined();expect(runtime.tagListeners.size).toBe(1);unmount(renderer);await settleReact();
    expect(worker.terminated).toBe(1);expect(runtime.tagUnsubscribes).toBe(1);expect(runtime.tagListeners.size).toBe(0);
    expect(runtime.tagCaptureDisables).toBeGreaterThan(0);expect(panelHarness.saveSessionRegistration).not.toHaveBeenCalled();
  });

  it('returns tag workers and frame subscribers to baseline through 100 mount/unmount cycles',async()=>{
    for(let index=0;index<100;index+=1){
      const renderer=await mountPanel({objectId:`OBJ_${index}`,sessionId:`SESSION_${index}`});const runtime=panelHarness.runtimes.at(-1);
      await startGeometry(runtime);await act(async()=>{tagButton(renderer).props.onClick();await settleReact();});
      const worker=panelHarness.workers.at(-1);expect(worker).toBeDefined();expect(runtime.tagListeners.size).toBe(1);
      unmount(renderer);expect(worker.terminated).toBe(1);expect(runtime.tagUnsubscribes).toBe(1);expect(runtime.tagListeners.size).toBe(0);
    }
    expect(panelHarness.workers).toHaveLength(100);expect(panelHarness.saveSessionRegistration).not.toHaveBeenCalled();
  });

  it('cancels pending detection on unmount and never publishes its late result',async()=>{
    const registration=vi.fn();const renderer=await mountPanel({objectId:'OBJ_A',sessionId:'SESSION_A',onRegistrationChange:registration});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    await act(async()=>{tagButton(renderer).props.onClick();await settleReact();});const worker=panelHarness.workers[0];
    runtime.emitTagFrame(tagFrame());
    expect(worker.messages.some((message:any)=>message.type==='detect')).toBe(true);registration.mockClear();unmount(renderer);
    emitTagDetection(worker);await settleReact();expect(registration).not.toHaveBeenCalled();expect(panelHarness.saveSessionRegistration).not.toHaveBeenCalled();
    expect(worker.terminated).toBe(1);expect(runtime.tagListeners.size).toBe(0);
  });

  it('hands the Finished scan over at once, delivers the tag result separately and never saves registration itself',async()=>{
    const registration=vi.fn();const tagResult=vi.fn();const ready=vi.fn();const renderer=await mountPanel({objectId:'OBJ_A',sessionId:'SESSION_A',onRegistrationChange:registration,onPointCloudReady:ready,onTagRegistrationResult:tagResult});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    await act(async()=>{tagButton(renderer).props.onClick();await settleReact();});const worker=panelHarness.workers[0];
    runtime.emitTagFrame(tagFrame());emitTagDetection(worker);await act(async()=>{await settleReact();});
    registration.mockClear();ready.mockClear();
    await act(async()=>{runtime.setPhase('ended',{pointCount:1});await settleReact();});await act(async()=>{await settleReact();});
    expect(ready).toHaveBeenCalledTimes(1);expect(ready).toHaveBeenCalledWith(runtime.finalSnapshot);
    expect(tagResult).toHaveBeenCalledTimes(1);expect(tagResult.mock.calls[0][1]).toBe(true);expect(tagResult.mock.calls[0][0]).toMatchObject({schema:1,scan_frame_id:null});
    expect(ready.mock.invocationCallOrder[0]).toBeLessThan(tagResult.mock.invocationCallOrder[0]);
    expect(panelHarness.saveSessionRegistration).not.toHaveBeenCalled();expect(registration).not.toHaveBeenCalled();unmount(renderer);
  });

  it('uses camera pose from the captured frame when detector results arrive after the XR pose changes',async()=>{
    const fusionSpy=vi.spyOn(tagObservationFusion,'fuseTagObservations');
    const renderer=await mountPanel({objectId:'OBJ_A',sessionId:'SESSION_A'});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    await act(async()=>{tagButton(renderer).props.onClick();await settleReact();});const worker=panelHarness.workers[0];
    runtime.emitTagFrame(tagFrame(10,[0,0,0]));
    await act(async()=>{runtime.setPhase('running',{tracking:true,pose:{position:[10,0,0],orientation:[0,0,0,1],views:1}});emitTagDetection(worker);await settleReact();});
    expect(fusionSpy.mock.calls[0][0][0]).toMatchObject({capturedAtMs:10,cameraPosition:[0,0,0],cameraForward:[0,0,-1]});
    runtime.emitTagFrame(tagFrame(20,[0,0,0]));
    await act(async()=>{runtime.setPhase('running',{tracking:true,pose:{position:[20,0,0],orientation:[0,0,0,1],views:1}});emitTagDetection(worker);await settleReact();});
    const chips=renderer.root.findAll((node)=>node.props['aria-label']===translate('en','xr.tag.observations'))[0];
    expect(textOf(chips)).toContain(translate('en','xr.tag.chip.views_seen',{node:'MAIN',views:'1',minimum:'3'}));
    unmount(renderer);
  });

  it('does not let a repeated same-view outlier dominate diverse observations',async()=>{
    const tagResult=vi.fn();const renderer=await mountPanel({objectId:'OBJ_A',sessionId:'SESSION_A',onTagRegistrationResult:tagResult});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    await act(async()=>{tagButton(renderer).props.onClick();await settleReact();});const worker=panelHarness.workers[0];
    for(let index=0;index<5;index+=1){runtime.emitTagFrame(tagFrame(index+1,[0.3+index*0.3,0,0]));await act(async()=>{emitTagDetection(worker,{positionX:1});await settleReact();});}
    for(let repeat=0;repeat<20;repeat+=1){runtime.emitTagFrame(tagFrame(100+repeat,[10,0,0]));await act(async()=>{emitTagDetection(worker,{positionX:100});await settleReact();});}
    await act(async()=>{runtime.setPhase('ended',{pointCount:1});await settleReact();});await act(async()=>{await settleReact();});
    expect(tagResult).toHaveBeenCalledTimes(1);
    expect(tagResult.mock.calls[0][0].bindings[0]).toMatchObject({tag_id:0,observation_count:6,inlier_count:5,position_scan_m:[1,2,3]});
    unmount(renderer);
  });

  it('keeps panel fusion input and retained observations bounded through a long tag session',async()=>{
    const original=TagObservationStore.prototype.getObservations;const retainedCounts:number[]=[];
    vi.spyOn(TagObservationStore.prototype,'getObservations').mockImplementation(function(this:TagObservationStore,tagId?:number){
      const observations=original.call(this,tagId);retainedCounts.push(this.retainedCount);return observations;
    });
    const fuseSpy=vi.spyOn(tagObservationFusion,'fuseTagObservations');
    const renderer=await mountPanel({objectId:'OBJ_A',sessionId:'SESSION_A'});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    await act(async()=>{tagButton(renderer).props.onClick();await settleReact();});const worker=panelHarness.workers[0];
    for(let index=0;index<60;index+=1){runtime.emitTagFrame(tagFrame(index*330,[index*0.2,0,0]));await act(async()=>{emitTagDetection(worker,{positionX:1});await settleReact();});}
    expect(retainedCounts).toHaveLength(60);expect(Math.max(...retainedCounts)).toBeLessThanOrEqual(48);
    expect(retainedCounts.slice(-10)).toEqual(Array(10).fill(48));
    const fusionInputLengths=fuseSpy.mock.calls.map(([observations])=>observations.length);
    expect(fusionInputLengths).toHaveLength(60);expect(Math.max(...fusionInputLengths)).toBeLessThanOrEqual(48);
    expect(fusionInputLengths.slice(-10)).toEqual(Array(10).fill(48));
    unmount(renderer);
  });

  it('does not wait for a stalled detector to hand over the scan, then cancels it after the bounded window',async()=>{
    vi.useFakeTimers();
    try{
      const ready=vi.fn();const tagResult=vi.fn();const renderer=await mountPanel({objectId:'OBJ_A',sessionId:'SESSION_A',onPointCloudReady:ready,onTagRegistrationResult:tagResult});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
      await act(async()=>{tagButton(renderer).props.onClick();await settleReact();});const worker=panelHarness.workers[0];
      runtime.emitTagFrame(tagFrame());
      await act(async()=>{runtime.setPhase('ended',{pointCount:1});await settleReact();});
      expect(ready).toHaveBeenCalledWith(runtime.finalSnapshot);expect(tagResult).not.toHaveBeenCalled();expect(worker.terminated).toBe(0);
      await act(async()=>{await vi.advanceTimersByTimeAsync(2999);await settleReact();});expect(tagResult).not.toHaveBeenCalled();
      await act(async()=>{await vi.advanceTimersByTimeAsync(2);await settleReact();});
      expect(worker.terminated).toBe(1);expect(tagResult).toHaveBeenCalledTimes(1);expect(tagResult.mock.calls[0][1]).toBe(true);
      emitTagDetection(worker);await act(async()=>{await settleReact();});expect(tagResult).toHaveBeenCalledTimes(1);
      expect(panelHarness.saveSessionRegistration).not.toHaveBeenCalled();unmount(renderer);
    }finally{vi.useRealTimers();}
  });

  it('never promotes an interrupted session: no scan hand-over and no tag result',async()=>{
    const ready=vi.fn();const tagResult=vi.fn();const renderer=await mountPanel({objectId:'OBJ_A',sessionId:'SESSION_A',onPointCloudReady:ready,onTagRegistrationResult:tagResult});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    ready.mockClear();await act(async()=>{runtime.setPhase('interrupted',{pointCount:5});await settleReact();});await act(async()=>{await settleReact();});
    expect(ready).not.toHaveBeenCalled();expect(tagResult).not.toHaveBeenCalled();expect(panelHarness.saveSessionRegistration).not.toHaveBeenCalled();unmount(renderer);
  });

  it('does not restore or republish the previous registration for a Fresh scan',async()=>{
    panelHarness.loadSessionRegistration.mockResolvedValue(registrationJson('OLD'));const callback=vi.fn();
    const renderer=await mountPanel({scanIntent:'fresh',baseScanFrameId:BASE_ID,objectId:'OBJ_A',sessionId:'SESSION_A',onRegistrationChange:callback});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    expect(panelHarness.loadSessionRegistration).not.toHaveBeenCalled();expect(callback).not.toHaveBeenCalled();unmount(renderer);
  });

  it('republishes a Continue registration only when bound to the base scan and the frozen geometry',async()=>{
    const cases:Array<[string,Record<string,unknown>,string|null]>=[
      ['applicable',{},null],
      ['geometry mismatch',{geometry_computed_at_ms:999},translate('en','xr.tag.registration.reason.geometry_mismatch')],
      ['wrong scan generation',{scan_frame_id:'scan-crc32:bbbbbbbb'},translate('en','xr.tag.registration.reason.scan_mismatch')],
      ['legacy unbound',{scan_frame_id:null},translate('en','xr.tag.registration.reason.scan_unbound')],
      ['over threshold',{residual_rms_m:0.3},translate('en','xr.tag.registration.reason.quality')],
    ];
    for(const [,patch,reason] of cases){
      panelHarness.runtimes=[];panelHarness.loadSessionRegistration.mockResolvedValue(registrationJson('S',patch));const callback=vi.fn();
      const renderer=await mountPanel({...CONTINUE,objectId:'OBJ_A',sessionId:'S',onRegistrationChange:callback});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
      const notices=renderer.root.findAll((node)=>node.props.role==='status').map(textOf).join(' ');
      expect(callback).toHaveBeenCalledTimes(1);
      if(reason===null){expect(callback.mock.calls[0][0]).toMatchObject({session:'S'});expect(notices).not.toContain('was not applied');}
      else{expect(callback.mock.calls[0][0]).toBeNull();expect(notices).toContain(reason);}
      unmount(renderer);
    }
  });

  it('does not issue a registration request when geometry resolves after unmount',async()=>{
    const pending=deferred<ReturnType<typeof listing>>();panelHarness.fetchLocalizationRuns.mockReturnValue(pending.promise);const callback=vi.fn();
    const renderer=await mountPanel({...CONTINUE,objectId:'OBJ_A',sessionId:'SESSION_A',onRegistrationChange:callback});const runtime=panelHarness.runtimes[0];
    await act(async()=>{runtime.setPhase('starting');await settleReact();});expect(panelHarness.fetchLocalizationRuns).toHaveBeenCalledTimes(1);
    unmount(renderer);pending.resolve(listing());await settleReact();expect(panelHarness.loadSessionRegistration).not.toHaveBeenCalled();expect(callback).not.toHaveBeenCalled();
  });

  it('ignores a deferred registration response after unmount',async()=>{
    const pending=deferred<string|null>();panelHarness.loadSessionRegistration.mockReturnValue(pending.promise);const callback=vi.fn();
    const renderer=await mountPanel({...CONTINUE,objectId:'OBJ_A',sessionId:'SESSION_A',onRegistrationChange:callback});const runtime=panelHarness.runtimes[0];
    await act(async()=>{runtime.setPhase('starting');await settleReact();});expect(panelHarness.loadSessionRegistration).toHaveBeenCalledTimes(1);
    unmount(renderer);callback.mockClear();pending.resolve(registrationJson('A'));await settleReact();expect(callback).not.toHaveBeenCalled();
  });

  it('keeps session B geometry and registration when session A resolves last',async()=>{
    const aGeometry=deferred<ReturnType<typeof listing>>();const callbackA=vi.fn();const callbackB=vi.fn();
    panelHarness.fetchLocalizationRuns.mockImplementation((_objectId:string,sessionId:string)=>sessionId==='A'?aGeometry.promise:Promise.resolve(listing()));
    panelHarness.loadSessionRegistration.mockImplementation(async(_objectId:string,sessionId:string)=>registrationJson(sessionId));
    let renderer=await mountPanel({...CONTINUE,objectId:'OBJ_A',sessionId:'A',onRegistrationChange:callbackA});const runtime=panelHarness.runtimes[0];
    await act(async()=>{runtime.setPhase('starting');await settleReact();});
    await act(async()=>{renderer.update(createElement(LocaleProvider,{initialLocale:'en'},createElement(XrRuntimePanel,{...CONTINUE,objectId:'OBJ_B',sessionId:'B',onRegistrationChange:callbackB})));await settleReact();});
    await act(async()=>{await settleReact();});expect(panelHarness.fetchLocalizationRuns).toHaveBeenCalledTimes(2);
    aGeometry.resolve(listing());await act(async()=>{await settleReact();});
    expect(panelHarness.loadSessionRegistration.mock.calls.map((call)=>call[1])).toEqual(['B']);
    expect(callbackA).not.toHaveBeenCalled();expect(callbackB).toHaveBeenCalledTimes(1);expect(callbackB.mock.calls[0][0]).toMatchObject({session:'B'});
    unmount(renderer);
  });

  it('does not let session A registration reach session B callback when A resolves last',async()=>{
    const aRegistration=deferred<string|null>();const callbackA=vi.fn();const callbackB=vi.fn();
    panelHarness.loadSessionRegistration.mockImplementation((_objectId:string,sessionId:string)=>sessionId==='A'?aRegistration.promise:Promise.resolve(registrationJson(sessionId)));
    let renderer=await mountPanel({...CONTINUE,objectId:'OBJ_A',sessionId:'A',onRegistrationChange:callbackA});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    expect(panelHarness.loadSessionRegistration.mock.calls.map((call)=>call[1])).toEqual(['A']);
    await act(async()=>{renderer.update(createElement(LocaleProvider,{initialLocale:'en'},createElement(XrRuntimePanel,{...CONTINUE,objectId:'OBJ_B',sessionId:'B',onRegistrationChange:callbackB})));await settleReact();});
    expect(callbackA).not.toHaveBeenCalled();expect(callbackB).toHaveBeenCalledTimes(1);expect(callbackB.mock.calls[0][0]).toMatchObject({session:'B'});
    aRegistration.resolve(registrationJson('A'));await act(async()=>{await settleReact();});
    expect(callbackA).not.toHaveBeenCalled();expect(callbackB).toHaveBeenCalledTimes(1);unmount(renderer);
  });

  it('does not apply a stale registration error after switching sessions',async()=>{
    const oldRegistration=deferred<string|null>();const callback=vi.fn();
    panelHarness.loadSessionRegistration.mockImplementation((_objectId:string,sessionId:string)=>sessionId==='A'?oldRegistration.promise:Promise.resolve(registrationJson('B')));
    let renderer=await mountPanel({...CONTINUE,objectId:'OBJ_A',sessionId:'A',onRegistrationChange:callback});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    await act(async()=>{renderer.update(createElement(LocaleProvider,{initialLocale:'en'},createElement(XrRuntimePanel,{...CONTINUE,objectId:'OBJ_B',sessionId:'B',onRegistrationChange:callback})));await settleReact();});
    expect(callback).toHaveBeenCalledTimes(1);expect(callback.mock.calls[0][0]).toMatchObject({session:'B'});
    oldRegistration.reject(new Error('retired registration response'));await act(async()=>{await settleReact();});
    expect(renderer.root.findAll((node)=>node.props.role==='alert').map(textOf).join(' ')).not.toContain('retired registration');unmount(renderer);
  });

  it('survives 100 delayed open/close/session-change cycles without stale loads or callbacks',async()=>{
    const callback=vi.fn();
    for(let index=0;index<100;index+=1){
      const pendingA=deferred<ReturnType<typeof listing>>();const pendingB=deferred<ReturnType<typeof listing>>();
      panelHarness.fetchLocalizationRuns.mockReturnValueOnce(pendingA.promise).mockReturnValueOnce(pendingB.promise);
      let renderer=await mountPanel({objectId:`OBJ_A_${index}`,sessionId:`SESSION_A_${index}`,onRegistrationChange:callback});const runtime=panelHarness.runtimes.at(-1);
      await act(async()=>{runtime.setPhase('starting');await settleReact();});
      await act(async()=>{renderer.update(createElement(LocaleProvider,{initialLocale:'en'},createElement(XrRuntimePanel,{objectId:`OBJ_B_${index}`,sessionId:`SESSION_B_${index}`,onRegistrationChange:callback})));await settleReact();});
      unmount(renderer);pendingA.reject(new Error('retired geometry A'));pendingB.reject(new Error('retired geometry B'));await settleReact();
    }
    expect(panelHarness.fetchLocalizationRuns).toHaveBeenCalledTimes(200);expect(panelHarness.loadSessionRegistration).not.toHaveBeenCalled();expect(callback).not.toHaveBeenCalled();
  });

  it.each(['ended','interrupted'])('stops BLE on %s and prevents later advertisements',async(phase)=>{
    const bluetooth=new PanelBluetooth();vi.stubGlobal('navigator',{bluetooth});browserWindow.isSecureContext=true;
    const renderer=await mountPanel();const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    const bleButton=renderer.root.findAllByType('button').find((button)=>textOf(button)===translate('en','xr.ble_hints_off'));
    if(bleButton===undefined)throw new Error('Missing BLE hints button.');await act(async()=>{bleButton.props.onClick();await settleReact();});
    expect(bluetooth.liveScans).toBe(1);expect(bluetooth.listeners.size).toBe(1);
    await act(async()=>{runtime.setPhase(phase);await settleReact();});expect(bluetooth.scanStops).toBe(1);expect(bluetooth.listeners.size).toBe(0);
    bluetooth.advertise();expect(bluetooth.liveScans).toBe(0);unmount(renderer);
  });

  it('keeps BLE disabled outside a live XR session',async()=>{
    const bluetooth=new PanelBluetooth();vi.stubGlobal('navigator',{bluetooth});browserWindow.isSecureContext=true;
    const renderer=await mountPanel();
    const bleButton=renderer.root.findAllByType('button').find((button)=>textOf(button)===translate('en','xr.ble_hints_off'));
    if(bleButton===undefined)throw new Error('Missing BLE hints button.');expect(bleButton.props.disabled).toBe(true);
    await act(async()=>{await bleButton.props.onClick();await settleReact();});expect(bluetooth.requestCalls).toBe(0);unmount(renderer);
  });

  it('stops a pending BLE permission request when the XR session ends',async()=>{
    const bluetooth=new PanelBluetooth();const pending=deferred<{stop():void}>();bluetooth.nextRequest=pending.promise;vi.stubGlobal('navigator',{bluetooth});browserWindow.isSecureContext=true;
    const renderer=await mountPanel();const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    const bleButton=renderer.root.findAllByType('button').find((button)=>textOf(button)===translate('en','xr.ble_hints_off'));
    if(bleButton===undefined)throw new Error('Missing BLE hints button.');await act(async()=>{bleButton.props.onClick();await settleReact();});expect(bluetooth.requestCalls).toBe(1);
    await act(async()=>{runtime.setPhase('ended');await settleReact();});const late=bluetooth.createScan();pending.resolve(late);await act(async()=>{await settleReact();});
    expect(bluetooth.scanStops).toBe(1);expect(bluetooth.liveScans).toBe(0);expect(bluetooth.listeners.size).toBe(0);unmount(renderer);
  });

  it('returns BLE handles and listeners to baseline through 100 delayed panel unmounts',async()=>{
    browserWindow.isSecureContext=true;
    for(let index=0;index<100;index+=1){
      const bluetooth=new PanelBluetooth();const pending=deferred<{stop():void}>();bluetooth.nextRequest=pending.promise;vi.stubGlobal('navigator',{bluetooth});
      const renderer=await mountPanel();const runtime=panelHarness.runtimes.at(-1);
      await act(async()=>{runtime.setPhase('running');await settleReact();});
      const button=renderer.root.findAllByType('button').find((candidate)=>textOf(candidate)===translate('en','xr.ble_hints_off'));
      if(button===undefined)throw new Error('Missing BLE hints button.');await act(async()=>{button.props.onClick();await settleReact();});
      expect(bluetooth.listeners.size).toBe(1);unmount(renderer);const late=bluetooth.createScan();pending.resolve(late);await settleReact();bluetooth.advertise();
      expect(bluetooth.scanStops).toBe(1);expect(bluetooth.liveScans).toBe(0);expect(bluetooth.listeners.size).toBe(0);
    }
  });
});
