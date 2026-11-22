import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createElement, StrictMode, type ComponentProps } from 'react';
import { act, create, type ReactTestInstance, type ReactTestRenderer } from 'react-test-renderer';

const panelHarness=vi.hoisted(()=>({
  runtimes:[] as any[],workers:[] as any[],
  fetchLocalizationRuns:vi.fn(),loadSessionRegistration:vi.fn(),saveSessionRegistration:vi.fn(),probeCapabilities:vi.fn(),
  makeState:(()=>({})) as (()=>any)|null,panelRenders:0,
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
    detachedAfterStart=0;attach=()=>{this.attachCalls+=1;this.attached=true;};detach=async()=>{this.detachCalls+=1;if(this.generation>0)this.detachedAfterStart+=1;this.attached=false;};getState=()=>this.state;getLifecycleGeneration=()=>this.generation;
    subscribe=(listener:(state:any)=>void)=>{this.listeners.add(listener);listener(this.state);return()=>this.listeners.delete(listener);};
    subscribeTagFrames=(listener:(frame:any)=>void)=>{this.tagListeners.add(listener);return()=>{this.tagListeners.delete(listener);this.tagUnsubscribes+=1;};};
    emitTagFrame=(frame:any)=>{for(const listener of this.tagListeners)listener(frame);};
    tagCaptureEnables=0;setTagCaptureEnabled=(enabled:boolean)=>{if(enabled)this.tagCaptureEnables+=1;else this.tagCaptureDisables+=1;return null;};
    getPointCloudSnapshot=()=>this.finalSnapshot;getLastManualPointId=()=>null;getStructuralGuideSnapshot=()=>({positions:new Float32Array(),normals:new Float32Array(),confidence:new Float32Array(),pointCount:0,keyframeCount:0,voxelSizeMeters:0});
    projectProjectPointToNormalized=()=>null;addSuggestedManualPoint=()=>null;queueManualPointAtNormalized=()=>undefined;undoLastManualPoint=()=>undefined;
    private publish(phase:string,patch:Record<string,unknown>={}){this.state={...this.state,...patch,phase};for(const listener of this.listeners)listener(this.state);}
    setPhase=(phase:string,patch:Record<string,unknown>={})=>{if(['ended','interrupted','error'].includes(phase)&&this.state.phase!==phase)this.generation+=1;this.publish(phase,patch);};
    depthGate:boolean|null=null;depthGateCalls:boolean[]=[];setDepthGate=(blocked:boolean)=>{this.depthGate=blocked;this.depthGateCalls.push(blocked);};
    referenceOverlay:any=null;overlayCalls:any[]=[];setReferenceOverlay=(overlay:any)=>{this.referenceOverlay=overlay;this.overlayCalls.push(overlay);};
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
vi.mock('../../src/components/StructuralGuideOverlay',()=>({StructuralGuideOverlay:()=>{panelHarness.panelRenders+=1;return null;}}));/* counts panel renders: a non-memo child renders whenever the panel does */
vi.mock('../../src/components/LiveTagOverlay',()=>({LiveTagOverlay:()=>null}));
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
  return{...original,estimateTagPose:(input:any)=>({ok:true,positionScan:[Number(input.corners[0][0]),2,3+Number(input.corners[0][1])],capturedAtMs:input.capturedAtMs,cameraPosition:input.cameraPosition,cameraForward:input.cameraForward})};
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
import { clientDiagnostics } from '../../src/data/clientDiagnostics';
import { liveComputeScheduler } from '../../src/xr/liveComputeScheduler';
import { optionalMonocularPrior } from '../../src/xr/monocularPriorRuntime';
import { clearScanLaunchIntent, hasScanLaunchIntent, requestScanLaunch, SCAN_LAUNCH_STAGING_SESSION } from '../../src/xr/scanLaunchIntent';

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
  return{phase,referenceSpace:null,domOverlay:false,environmentBlendMode:null,frameCount:0,tracking:false,pose:null,scanPaused:false,wakeLockActive:false,wakeLockError:null,depthAvailable:false,depthUsage:null,depthDataFormat:null,depthType:null,depthWidth:null,depthHeight:null,depthFrameCount:0,sampledDepthFrames:0,pointCount:0,retainedPointCount:0,pointCloudVersion:0,keyframeCount:0,polishFrameCount:0,motionRejectedFrames:0,motionState:null,motionQuality:0,linearSpeedMetersPerSecond:0,angularSpeedRadiansPerSecond:0,linearAccelerationMetersPerSecond2:0,coverageGuideActive:false,coverageGuideRenderPoints:0,coverageVisibleCells:0,coverageReadyCells:0,coverageTotalCells:0,coverageTargetIndependentViews:3,coverageAverageConfidence:0,coverageWeakPoints:0,coverageImprovingPoints:0,coverageStablePoints:0,coverageGuideError:null,manualPointCount:0,manualEdgeCount:0,manualPointError:null,manualPointErrorKey:null,performance:{pressure:'low',adaptiveCoverageIntervalMs:550,frameDeltaP95Ms:0,fusionP50Ms:0,fusionP95Ms:0,coverageP95Ms:0,longFrames33Ms:0,longFrames50Ms:0},lightingEstimateAvailable:false,primaryLightIntensity:null,rgbGuidanceAvailable:false,rgbGuidanceWidth:null,rgbGuidanceHeight:null,visualEvidenceFrames:0,monocularPriorStatus:'disabled',depthError:null,error:null,...patch};
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
  /* The title changes with the state: capture_title while off, tags_on once recognition runs (it now starts with the scan). */const titles=[translate('en','xr.tag.capture_title'),translate('en','xr.tag.tags_on')];const result=renderer.root.findAllByType('button').find((button)=>titles.includes(button.props.title));if(result===undefined)throw new Error('Missing AprilTag button.');return result;
}
function emitTagDetection(worker:any,options:{tagId?:number;positionX?:number;positionZ?:number}={}):void{
  const request=worker.messages.filter((item:any)=>item.type==='detect').at(-1);if(request===undefined)throw new Error('Expected an in-flight tag detection.');
  const x=options.positionX??1;const z=options.positionZ??0;worker.emit('message',{type:'result',requestId:request.requestId,epoch:request.epoch,detections:[{tagId:options.tagId??0,corners:[[x,z],[x+1,z],[x+1,z+1],[x,z+1]],hammingDistance:0}]});
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
  (globalThis as any).__runPanelTimeouts=()=>{for(const [id,callback] of [...timers]){timers.delete(id);callback();}};
  (globalThis as any).__runPanelIntervals=()=>{for(const callback of [...intervals.values()])callback();};
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
  it('renders a runtime capacity key as a localized AR overlay warning',async()=>{
    panelHarness.makeState=()=>runtimeState('running',{manualPointErrorKey:'xr.manual.pin_capacity_reached'});
    const renderer=await mountPanel();
    const warnings=renderer.root.findAll((node)=>node.props.className==='xr-overlay-warning').map(textOf);
    expect(warnings).toContain(translate('en','xr.manual.pin_capacity_reached'));
    unmount(renderer);
  });

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

  it('starts tag recognition by itself with the scan, needing neither a recording nor localization geometry',async()=>{
    const renderer=await mountPanel({});const runtime=panelHarness.runtimes[0];
    await startGeometry(runtime);await act(async()=>{await settleReact();});
    expect(panelHarness.fetchLocalizationRuns).not.toHaveBeenCalled();
    expect(runtime.tagCaptureEnables).toBe(1);expect(runtime.tagListeners.size).toBe(1);expect(panelHarness.workers).toHaveLength(1);
    expect(tagButton(renderer).props['aria-pressed']).toBe(true);expect(tagButton(renderer).props.disabled).toBe(false);
    const overlayText=renderer.root.findAll((node)=>typeof node.props.className==='string'&&node.props.className.includes('xr-overlay-')).map(textOf).join(' ');
    expect(overlayText).not.toContain(translate('en','xr.tag.open_recording'));expect(overlayText).not.toContain(translate('en','xr.tag.geometry_unavailable'));
    unmount(renderer);
  });

  it('decodes tags for a scan without a recording and finishes it with no tag error and no registration',async()=>{
    const ready=vi.fn();const tagResult=vi.fn();
    const renderer=await mountPanel({onPointCloudReady:ready,onTagRegistrationResult:tagResult});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    await act(async()=>{await settleReact();});const worker=panelHarness.workers[0];
    runtime.emitTagFrame(tagFrame());emitTagDetection(worker);await act(async()=>{await settleReact();});
    ready.mockClear();
    await act(async()=>{runtime.setPhase('ended',{pointCount:1});await settleReact();});await act(async()=>{await settleReact();});
    expect(ready).toHaveBeenCalledTimes(1);expect(ready).toHaveBeenCalledWith(runtime.finalSnapshot);
    expect(tagResult).toHaveBeenCalledTimes(1);expect(tagResult.mock.calls[0][0]).toBeNull();expect(tagResult.mock.calls[0][1]).toBe(true);
    const warnings=renderer.root.findAll((node)=>node.props.className==='xr-overlay-warning').map(textOf).join(' ');
    expect(warnings).not.toContain(translate('en','xr.tag.error.geometry_unavailable'));
    expect(panelHarness.saveSessionRegistration).not.toHaveBeenCalled();unmount(renderer);
  });

  it('hands every tag position to the result view when the scan has no geometry, flags the unconfirmed ones, and saves no registration',async()=>{
    const tagResult=vi.fn();const anchors=vi.fn();
    const renderer=await mountPanel({onTagRegistrationResult:tagResult,onTagAnchors:anchors});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    await act(async()=>{await settleReact();});const worker=panelHarness.workers[0];
    const observe=async(tagId:number,count:number,firstIndex:number)=>{
      for(let index=0;index<count;index+=1){runtime.emitTagFrame(tagFrame(firstIndex+index,[(firstIndex+index)*0.3,0,0]));await act(async()=>{emitTagDetection(worker,{tagId,positionX:tagId+1});await settleReact();});}
    };
    await observe(0,6,1);   /* MAIN: 6 observations from different places -> confirmed */
    await observe(2,2,10);  /* G2: seen twice -> not confirmed */
    expect(anchors).not.toHaveBeenCalled();
    await act(async()=>{runtime.setPhase('ended',{pointCount:1});await settleReact();});await act(async()=>{await settleReact();});
    expect(anchors).toHaveBeenCalledTimes(1);
    expect(anchors.mock.calls[0][0].map((anchor:any)=>[anchor.tagId,anchor.confirmed])).toEqual([[0,true],[2,false]]);
    expect(anchors.mock.calls[0][0][0]).toMatchObject({node:'MAIN',observationCount:6});
    expect(anchors.mock.calls[0][0][1]).toMatchObject({node:'G2',observationCount:2});
    expect(tagResult).toHaveBeenCalledTimes(1);expect(tagResult.mock.calls[0][0]).toBeNull();   /* no geometry: nothing registered */
    expect(panelHarness.saveSessionRegistration).not.toHaveBeenCalled();
    unmount(renderer);
  });

  it('hands over no anchors for a scan that was interrupted',async()=>{
    const anchors=vi.fn();
    const renderer=await mountPanel({onTagAnchors:anchors});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    await act(async()=>{await settleReact();});const worker=panelHarness.workers[0];
    for(let index=1;index<=6;index+=1){runtime.emitTagFrame(tagFrame(index,[index*0.3,0,0]));await act(async()=>{emitTagDetection(worker,{tagId:0,positionX:1});await settleReact();});}
    await act(async()=>{runtime.setPhase('interrupted',{pointCount:1});await settleReact();});await act(async()=>{await settleReact();});
    expect(anchors).not.toHaveBeenCalled();
    unmount(renderer);
  });

  it('keeps tags off for the rest of the scan once the operator switches them off, and restores them with the next scan',async()=>{
    const renderer=await mountPanel({});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    await act(async()=>{await settleReact();});expect(runtime.tagListeners.size).toBe(1);
    await act(async()=>{tagButton(renderer).props.onClick();await settleReact();});
    expect(tagButton(renderer).props['aria-pressed']).toBe(false);expect(runtime.tagListeners.size).toBe(0);expect(runtime.tagCaptureDisables).toBeGreaterThan(0);
    await act(async()=>{runtime.setPhase('running',{frameCount:50});await settleReact();});
    expect(runtime.tagListeners.size).toBe(0);expect(runtime.tagCaptureEnables).toBe(1);
    await act(async()=>{runtime.setPhase('ended',{pointCount:1});await settleReact();});await startGeometry(runtime);await act(async()=>{await settleReact();});
    expect(runtime.tagListeners.size).toBe(1);expect(runtime.tagCaptureEnables).toBe(2);expect(tagButton(renderer).props['aria-pressed']).toBe(true);
    unmount(renderer);
  });

  it('keeps the Tags switch and every tag status out of the camera view',async()=>{
    const renderer=await mountPanel({});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);await act(async()=>{await settleReact();});
    const worker=panelHarness.workers[0];runtime.emitTagFrame(tagFrame());emitTagDetection(worker);await act(async()=>{await settleReact();});
    const titles=[translate('en','xr.tag.capture_title'),translate('en','xr.tag.tags_on')];
    const overlay=renderer.root.findAll((node)=>node.props.className==='xr-immersive-overlay')[0];
    expect(overlay.findAllByType('button').filter((button)=>titles.includes(button.props.title))).toHaveLength(0);
    expect(renderer.root.findAll((node)=>node.props['aria-label']===translate('en','xr.tag.observations'))).toHaveLength(0);
    const details=renderer.root.findAll((node)=>node.props.className==='xr-technical-details')[0];
    expect(details.findAllByType('button').filter((button)=>titles.includes(button.props.title))).toHaveLength(1);
    expect(tagButton(renderer).props['aria-pressed']).toBe(true);
    unmount(renderer);
  });

  it('lets a device announcement fade by itself instead of staying over the camera',async()=>{
    const renderer=await mountPanel({});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    await act(async()=>{await settleReact();});const worker=panelHarness.workers[0];
    const notices=()=>renderer.root.findAll((node)=>node.props.className==='xr-overlay-hint xr-tag-notice');
    for(let index=0;index<6;index+=1){runtime.emitTagFrame(tagFrame(index+1,[(index+1)*0.3,0,0]));await act(async()=>{emitTagDetection(worker,{tagId:0,positionX:1});await settleReact();});}
    expect(notices().length).toBeGreaterThan(0);
    await act(async()=>{(globalThis as any).__runPanelTimeouts();await settleReact();});
    expect(notices()).toHaveLength(0);
    unmount(renderer);
  });

  it('does not re-render the whole scanner panel for detections that confirm nothing new',async()=>{
    const renderer=await mountPanel({});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    await act(async()=>{await settleReact();});const worker=panelHarness.workers[0];
    runtime.emitTagFrame(tagFrame(1,[0.3,0,0]));await act(async()=>{emitTagDetection(worker,{tagId:0,positionX:1});await settleReact();});
    const before=panelHarness.panelRenders;
    for(let index=2;index<=4;index+=1){runtime.emitTagFrame(tagFrame(index,[index*0.3,0,0]));await act(async()=>{emitTagDetection(worker,{tagId:0,positionX:1});await settleReact();});}
    expect(panelHarness.panelRenders).toBe(before);
    unmount(renderer);
  });

  it('keeps the camera view free of developer figures and of the buttons nobody needs: only Finish is left to press',async()=>{
    const renderer=await mountPanel({});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);await act(async()=>{await settleReact();});
    const overlay=renderer.root.findAll((node)=>node.props.className==='xr-immersive-overlay')[0];
    expect(overlay.findAllByType('button').map((button)=>button.props.className)).toEqual(['xr-finish-button']);
    for(const className of ['xr-overlay-scan-grid xr-overlay-scan-grid-compact','xr-overlay-topline','xr-manual-status','xr-overlay-actions','xr-manual-hit-surface'])
      expect(renderer.root.findAll((node)=>node.props.className===className)).toHaveLength(0);
    const labels=[translate('en','xr.ai_check_on'),translate('en','xr.ai_check_off'),translate('en','xr.manual.mark_corners'),translate('en','xr.manual.undo_pin')];
    expect(renderer.root.findAllByType('button').filter((button)=>labels.includes(textOf(button)))).toHaveLength(0);
    unmount(renderer);
  });

  it('still tells the operator what matters while scanning: paused, too fast, move to a new viewpoint',async()=>{
    const renderer=await mountPanel({});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    const warnings=()=>renderer.root.findAll((node)=>node.props.className==='xr-overlay-warning'||node.props.className==='xr-overlay-hint').map(textOf).join(' | ');
    await act(async()=>{runtime.setPhase('running',{motionState:'too-fast'});await settleReact();});
    expect(warnings()).toContain(translate('en','xr.manual.move_slower'));
    await act(async()=>{runtime.setPhase('running',{motionState:'stationary'});await settleReact();});
    expect(warnings()).toContain(translate('en','xr.manual.move_viewpoint'));
    await act(async()=>{runtime.setPhase('running',{motionState:'ok',scanPaused:true});await settleReact();});
    expect(warnings()).toContain(translate('en','xr.manual.paused'));
    unmount(renderer);
  });

  it('arms the AI check by itself, without a switch',async()=>{
    optionalMonocularPrior.setEnabled(false);
    const renderer=await mountPanel({});
    expect(optionalMonocularPrior.isEnabled).toBe(true);
    unmount(renderer);
  });

  it('stages the AI frame only while the phone has headroom, and always allows it again once the scan is over',async()=>{
    const snapshot=(pressure:'low'|'medium'|'high',active:number,degraded=false)=>({budget:2,maximumBudget:2,effectiveBudget:2,pressure,degraded,active,structural:active,visual:0});
    const spy=vi.spyOn(liveComputeScheduler,'snapshot');
    const info=vi.spyOn(clientDiagnostics,'info');
    const renderer=await mountPanel({});const runtime=panelHarness.runtimes[0];
    spy.mockReturnValue(snapshot('low',1));
    await startGeometry(runtime);
    expect(optionalMonocularPrior.isStagingAllowed).toBe(true);       /* 1 of 2 workers busy: exactly half is free */
    spy.mockReturnValue(snapshot('low',2));
    await act(async()=>{(globalThis as any).__runPanelIntervals();await settleReact();});
    expect(optionalMonocularPrior.isStagingAllowed).toBe(false);      /* both busy */
    expect(info).toHaveBeenCalledWith('XR_AI_STAGING',expect.objectContaining({allowed:false,active:2,budget:2}));
    spy.mockReturnValue(snapshot('medium',0));
    await act(async()=>{(globalThis as any).__runPanelIntervals();await settleReact();});
    expect(optionalMonocularPrior.isStagingAllowed).toBe(false);      /* idle workers but the phone is under pressure */
    spy.mockReturnValue(snapshot('low',0,true));
    await act(async()=>{(globalThis as any).__runPanelIntervals();await settleReact();});
    expect(optionalMonocularPrior.isStagingAllowed).toBe(false);      /* throttled */
    spy.mockReturnValue(snapshot('low',0));
    await act(async()=>{(globalThis as any).__runPanelIntervals();await settleReact();});
    expect(optionalMonocularPrior.isStagingAllowed).toBe(true);
    spy.mockReturnValue(snapshot('high',2));
    await act(async()=>{(globalThis as any).__runPanelIntervals();await settleReact();});
    expect(optionalMonocularPrior.isStagingAllowed).toBe(false);
    await act(async()=>{runtime.setPhase('ended',{pointCount:1});await settleReact();});
    expect(optionalMonocularPrior.isStagingAllowed).toBe(true);       /* the result is being processed: no gate any more */
    spy.mockRestore();unmount(renderer);
  });

  it('writes the developer telemetry that used to be on screen: state, load, keyframes and what changed on which frame',async()=>{
    const info=vi.spyOn(clientDiagnostics,'info');
    const renderer=await mountPanel({});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    await act(async()=>{await settleReact();});
    const events=(name:string)=>info.mock.calls.filter((call)=>call[0]===name).map((call)=>call[1] as Record<string,unknown>);
    expect(events('XR_STATE_CHANGE').length).toBeGreaterThan(0);
    expect(String(events('XR_STATE_CHANGE')[0].changed)).toContain('tags:');
    await act(async()=>{runtime.setPhase('running',{frameCount:420,scanPaused:true,depthAvailable:true});await settleReact();});
    const change=events('XR_STATE_CHANGE').at(-1)!;
    expect(change.frame).toBe(420);expect(String(change.changed)).toContain('paused:false>true');expect(String(change.changed)).toContain('depth:');
    await act(async()=>{runtime.setPhase('running',{frameCount:530,depthFrameCount:500,sampledDepthFrames:60,pointCount:900,polishFrameCount:7,keyframeCount:12});await settleReact();});
    expect(events('XR_KEYFRAME').at(-1)).toMatchObject({n:12,frame:530,depthFrame:500,integrated:60,points:900,polish:7});
    await act(async()=>{runtime.setPhase('running',{frameCount:600,keyframeCount:13});await settleReact();});
    expect(events('XR_KEYFRAME').map((event)=>event.n)).toEqual([12,13]);
    await act(async()=>{(globalThis as any).__runPanelIntervals();await settleReact();});
    const viewport=events('XR_VIEWPORT_STATE').at(-1);const perf=events('XR_PERF_STATE').at(-1);
    expect(viewport).toMatchObject({f:600,kf:13,pts:900,pol:7,pause:true});
    expect(perf).toMatchObject({f:600});expect(perf).toHaveProperty('cpu');expect(perf).toHaveProperty('p95');expect(perf).toHaveProperty('l33');expect(perf).toHaveProperty('press');
    /* The client log keeps 420 characters per line: a longer record would be cut in the middle of its JSON. */
    for(const record of [viewport,perf,change])expect(JSON.stringify(record).length).toBeLessThan(380);
    unmount(renderer);
  });

  it('stops the periodic telemetry when the scan ends',async()=>{
    const info=vi.spyOn(clientDiagnostics,'info');
    const renderer=await mountPanel({});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);await act(async()=>{await settleReact();});
    await act(async()=>{runtime.setPhase('ended',{pointCount:1});await settleReact();});
    info.mockClear();
    await act(async()=>{(globalThis as any).__runPanelIntervals();await settleReact();});
    expect(info.mock.calls.filter((call)=>call[0]==='XR_VIEWPORT_STATE'||call[0]==='XR_PERF_STATE')).toHaveLength(0);
    unmount(renderer);
  });

  it('does not start recognition and says why when the phone exposes no camera access',async()=>{
    panelHarness.probeCapabilities.mockResolvedValue({...validCapabilities,rawXrRgbApi:false});
    const renderer=await mountPanel({});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);await act(async()=>{await settleReact();});
    expect(runtime.tagCaptureEnables).toBe(0);expect(runtime.tagListeners.size).toBe(0);expect(panelHarness.workers).toHaveLength(0);
    expect(tagButton(renderer).props.disabled).toBe(true);
    expect(renderer.root.findAll((node)=>node.props.className==='xr-overlay-hint').map(textOf).join(' ')).toContain(translate('en','xr.tag.camera_unavailable'));
    unmount(renderer);
  });

  it('tears down tag worker and subscription on unmount without saving registration',async()=>{
    const renderer=await mountPanel({objectId:'OBJ_A',sessionId:'SESSION_A'});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    expect(runtime.attached).toBe(true);
    await act(async()=>{await settleReact();expect(tagButton(renderer).props['aria-pressed']).toBe(true);});/* tags start by themselves with the scan */const worker=panelHarness.workers[0];
    expect(worker).toBeDefined();expect(runtime.tagListeners.size).toBe(1);unmount(renderer);await settleReact();
    expect(worker.terminated).toBe(1);expect(runtime.tagUnsubscribes).toBe(1);expect(runtime.tagListeners.size).toBe(0);
    expect(runtime.tagCaptureDisables).toBeGreaterThan(0);expect(panelHarness.saveSessionRegistration).not.toHaveBeenCalled();
  });

  it('returns tag workers and frame subscribers to baseline through 100 mount/unmount cycles',async()=>{
    for(let index=0;index<100;index+=1){
      const renderer=await mountPanel({objectId:`OBJ_${index}`,sessionId:`SESSION_${index}`});const runtime=panelHarness.runtimes.at(-1);
      await startGeometry(runtime);await act(async()=>{await settleReact();expect(tagButton(renderer).props['aria-pressed']).toBe(true);});/* tags start by themselves with the scan */
      const worker=panelHarness.workers.at(-1);expect(worker).toBeDefined();expect(runtime.tagListeners.size).toBe(1);
      unmount(renderer);expect(worker.terminated).toBe(1);expect(runtime.tagUnsubscribes).toBe(1);expect(runtime.tagListeners.size).toBe(0);
    }
    expect(panelHarness.workers).toHaveLength(100);expect(panelHarness.saveSessionRegistration).not.toHaveBeenCalled();
  });

  it('cancels pending detection on unmount and never publishes its late result',async()=>{
    const registration=vi.fn();const renderer=await mountPanel({objectId:'OBJ_A',sessionId:'SESSION_A',onRegistrationChange:registration});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    await act(async()=>{await settleReact();expect(tagButton(renderer).props['aria-pressed']).toBe(true);});/* tags start by themselves with the scan */const worker=panelHarness.workers[0];
    runtime.emitTagFrame(tagFrame());
    expect(worker.messages.some((message:any)=>message.type==='detect')).toBe(true);registration.mockClear();unmount(renderer);
    emitTagDetection(worker);await settleReact();expect(registration).not.toHaveBeenCalled();expect(panelHarness.saveSessionRegistration).not.toHaveBeenCalled();
    expect(worker.terminated).toBe(1);expect(runtime.tagListeners.size).toBe(0);
  });

  it('hands the Finished scan over at once, delivers the tag result separately and never saves registration itself',async()=>{
    const registration=vi.fn();const tagResult=vi.fn();const ready=vi.fn();const renderer=await mountPanel({objectId:'OBJ_A',sessionId:'SESSION_A',onRegistrationChange:registration,onPointCloudReady:ready,onTagRegistrationResult:tagResult});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    await act(async()=>{await settleReact();expect(tagButton(renderer).props['aria-pressed']).toBe(true);});/* tags start by themselves with the scan */const worker=panelHarness.workers[0];
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
    await act(async()=>{await settleReact();expect(tagButton(renderer).props['aria-pressed']).toBe(true);});/* tags start by themselves with the scan */const worker=panelHarness.workers[0];
    runtime.emitTagFrame(tagFrame(10,[0,0,0]));
    await act(async()=>{runtime.setPhase('running',{tracking:true,pose:{position:[10,0,0],orientation:[0,0,0,1],views:1}});emitTagDetection(worker);await settleReact();});
    expect(fusionSpy.mock.calls[0][0][0]).toMatchObject({capturedAtMs:10,cameraPosition:[0,0,0],cameraForward:[0,0,-1]});
    runtime.emitTagFrame(tagFrame(20,[0,0,0]));
    await act(async()=>{runtime.setPhase('running',{tracking:true,pose:{position:[20,0,0],orientation:[0,0,0,1],views:1}});emitTagDetection(worker);await settleReact();});
    /* Two detections from the same camera position are one independent view: not confirmed yet. (The status chips over the camera are gone.) */
    const latest=fusionSpy.mock.results.at(-1)?.value as any;
    expect(latest.byTagId[0]).toMatchObject({independentViewCount:1,confirmed:false});
    unmount(renderer);
  });

  it('does not let a repeated same-view outlier dominate diverse observations',async()=>{
    const tagResult=vi.fn();const renderer=await mountPanel({objectId:'OBJ_A',sessionId:'SESSION_A',onTagRegistrationResult:tagResult});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    await act(async()=>{await settleReact();expect(tagButton(renderer).props['aria-pressed']).toBe(true);});/* tags start by themselves with the scan */const worker=panelHarness.workers[0];
    for(let index=0;index<5;index+=1){runtime.emitTagFrame(tagFrame(index+1,[0.3+index*0.3,0,0]));await act(async()=>{emitTagDetection(worker,{positionX:1});await settleReact();});}
    for(let repeat=0;repeat<20;repeat+=1){runtime.emitTagFrame(tagFrame(100+repeat,[10,0,0]));await act(async()=>{emitTagDetection(worker,{positionX:100});await settleReact();});}
    await act(async()=>{runtime.setPhase('ended',{pointCount:1});await settleReact();});await act(async()=>{await settleReact();});
    expect(tagResult).toHaveBeenCalledTimes(1);
    expect(tagResult.mock.calls[0][0].bindings[0]).toMatchObject({tag_id:0,observation_count:6,inlier_count:5,position_scan_m:[1,2,3]});
    unmount(renderer);
  });

  it('announces a scanned device in the scan record and names the pairs whose distance is recalculated',async()=>{
    const renderer=await mountPanel({objectId:'OBJ_A',sessionId:'SESSION_A'});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    await act(async()=>{await settleReact();expect(tagButton(renderer).props['aria-pressed']).toBe(true);});/* tags start by themselves with the scan */const worker=panelHarness.workers[0];
    const notices=()=>renderer.root.findAll((node)=>node.props.className==='xr-overlay-hint xr-tag-notice');
    const observe=async(tagId:number,count:number,firstIndex:number)=>{
      for(let index=0;index<count;index+=1){runtime.emitTagFrame(tagFrame(firstIndex+index,[(firstIndex+index)*0.3,0,0]));await act(async()=>{emitTagDetection(worker,{tagId,positionX:tagId+1});await settleReact();});}
    };
    await observe(0,4,1);
    expect(notices()).toHaveLength(0);                                      /* seen, not confirmed yet: nothing is announced */
    await observe(0,1,5);
    expect(notices().map(textOf)).toEqual([translate('en','xr.tag.notice.first',{node:'MAIN'})]);
    expect(notices()[0].props['data-node']).toBe('MAIN');
    await observe(2,5,10);
    expect(notices().map(textOf)).toEqual([
      translate('en','xr.tag.notice.first',{node:'MAIN'}),
      translate('en','xr.tag.notice.recalculating',{node:'G2',pairs:'G2–MAIN'}),
    ]);
    const region=renderer.root.findAll((node)=>node.props.className==='xr-tag-notices')[0];
    expect(region.props.role).toBe('status');expect(region.props['aria-live']).toBe('polite');
    await observe(2,3,20);
    expect(notices()).toHaveLength(2);                                      /* no repeat for a device that is already announced */
    unmount(renderer);
  });

  it('keeps panel fusion input and retained observations bounded through a long tag session',async()=>{
    const original=TagObservationStore.prototype.getObservations;const retainedCounts:number[]=[];
    vi.spyOn(TagObservationStore.prototype,'getObservations').mockImplementation(function(this:TagObservationStore,tagId?:number){
      const observations=original.call(this,tagId);retainedCounts.push(this.retainedCount);return observations;
    });
    const fuseSpy=vi.spyOn(tagObservationFusion,'fuseTagObservations');
    const renderer=await mountPanel({objectId:'OBJ_A',sessionId:'SESSION_A'});const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    await act(async()=>{await settleReact();expect(tagButton(renderer).props['aria-pressed']).toBe(true);});/* tags start by themselves with the scan */const worker=panelHarness.workers[0];
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
      await act(async()=>{await settleReact();expect(tagButton(renderer).props['aria-pressed']).toBe(true);});/* tags start by themselves with the scan */const worker=panelHarness.workers[0];
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

describe('one-tap scan launch (issue #21)',()=>{
  const live=()=>vi.stubGlobal('navigator',{userActivation:{isActive:true}});
  const starts=()=>panelHarness.runtimes.reduce((sum:number,runtime:any)=>sum+runtime.generation,0);
  beforeEach(()=>{clearScanLaunchIntent();});
  afterEach(()=>{clearScanLaunchIntent();});

  it('starts exactly one Fresh scan from the tap that opened the scanner, with no second Start tap',async()=>{
    live();expect(requestScanLaunch('OBJ_1','S_1')).toBe(true);
    const renderer=await mountPanel({objectId:'OBJ_1',sessionId:'S_1',scanLaunchReady:true});
    await act(async()=>{await settleReact();(globalThis as any).__runPanelTimeouts();await settleReact();});
    expect(starts()).toBe(1);
    expect(panelHarness.runtimes[0].state.phase).toBe('starting');
    expect(hasScanLaunchIntent()).toBe(false);
    unmount(renderer);
  });

  it('does not start when the tap carried no live activation: the Start button remains the way in',async()=>{
    vi.stubGlobal('navigator',{userActivation:{isActive:false}});
    expect(requestScanLaunch('OBJ_1','S_1')).toBe(false);
    const renderer=await mountPanel({objectId:'OBJ_1',sessionId:'S_1',scanLaunchReady:true});
    await act(async()=>{await settleReact();(globalThis as any).__runPanelTimeouts();await settleReact();});
    expect(starts()).toBe(0);
    expect(panelHarness.runtimes[0].state.phase).toBe('idle');
    expect(renderer.root.findAll((node)=>node.props.className==='xr-start-button').length).toBeGreaterThan(0);
    unmount(renderer);
  });

  it('waits for the saved-scan restore: nothing starts while it is pending, then one start when it found no scan',async()=>{
    live();requestScanLaunch('OBJ_1','S_1');
    const props={objectId:'OBJ_1',sessionId:'S_1'};
    const renderer=await mountPanel({...props,scanLaunchReady:false});
    await act(async()=>{await settleReact();(globalThis as any).__runPanelTimeouts();await settleReact();});
    expect(starts()).toBe(0);
    await act(async()=>{renderer.update(createElement(LocaleProvider,{initialLocale:'en'},createElement(XrRuntimePanel,{...props,scanLaunchReady:true})));await settleReact();});
    await act(async()=>{await settleReact();(globalThis as any).__runPanelTimeouts();await settleReact();});
    expect(starts()).toBe(1);
    unmount(renderer);
  });

  it('never starts over a saved scan: it opens as viewer and the intent is dropped',async()=>{
    live();requestScanLaunch('OBJ_1','S_1');
    const renderer=await mountPanel({objectId:'OBJ_1',sessionId:'S_1',scanLaunchReady:true,savedScanAvailable:true});
    await act(async()=>{await settleReact();(globalThis as any).__runPanelTimeouts();await settleReact();});
    expect(starts()).toBe(0);
    expect(hasScanLaunchIntent()).toBe(false);
    unmount(renderer);
  });

  it('ignores an intent for another session and an expired one',async()=>{
    live();requestScanLaunch('OBJ_1','S_OTHER');
    let renderer=await mountPanel({objectId:'OBJ_1',sessionId:'S_1',scanLaunchReady:true});
    await act(async()=>{await settleReact();(globalThis as any).__runPanelTimeouts();await settleReact();});
    expect(starts()).toBe(0);unmount(renderer);

    panelHarness.runtimes=[];
    requestScanLaunch('OBJ_1','S_1',Date.now()-60_000);
    renderer=await mountPanel({objectId:'OBJ_1',sessionId:'S_1',scanLaunchReady:true});
    await act(async()=>{await settleReact();(globalThis as any).__runPanelTimeouts();await settleReact();});
    expect(starts()).toBe(0);unmount(renderer);
  });

  it('cannot repeat: Strict Mode double effects and a later remount start a scan only once',async()=>{
    live();requestScanLaunch('OBJ_1','S_1');
    let renderer=await mountPanel({objectId:'OBJ_1',sessionId:'S_1',scanLaunchReady:true},true);
    await act(async()=>{await settleReact();(globalThis as any).__runPanelTimeouts();await settleReact();});
    expect(starts()).toBe(1);
    unmount(renderer);

    panelHarness.runtimes=[];
    renderer=await mountPanel({objectId:'OBJ_1',sessionId:'S_1',scanLaunchReady:true});
    await act(async()=>{await settleReact();(globalThis as any).__runPanelTimeouts();await settleReact();});
    expect(starts()).toBe(0);
    unmount(renderer);
  });
  it('does not start the scan in a Strict Mode run that is about to be cleaned up: the surviving runtime is started and never detached',async()=>{
    live();requestScanLaunch('OBJ_1',SCAN_LAUNCH_STAGING_SESSION);
    const renderer=await mountPanel({objectId:'OBJ_1',sessionId:null,scanLaunchReady:true},true);
    await act(async()=>{await settleReact();(globalThis as any).__runPanelTimeouts();await settleReact();});
    expect(starts()).toBe(1);
    const started=panelHarness.runtimes.filter((runtime:any)=>runtime.generation>0);
    expect(started).toHaveLength(1);
    expect(started[0].attached).toBe(true);
    expect(started[0].detachedAfterStart).toBe(0);
    unmount(renderer);
  });

  it('starts the staged workflow scan (no session id yet) from the intent left by the workflow tap',async()=>{
    live();expect(requestScanLaunch('OBJ_1',SCAN_LAUNCH_STAGING_SESSION)).toBe(true);
    const renderer=await mountPanel({objectId:'OBJ_1',sessionId:null,scanLaunchReady:true});
    await act(async()=>{await settleReact();(globalThis as any).__runPanelTimeouts();await settleReact();});
    expect(starts()).toBe(1);
    expect(hasScanLaunchIntent()).toBe(false);
    unmount(renderer);
  });

  it('does not start the staged scan for an intent of another object',async()=>{
    live();requestScanLaunch('OBJ_OTHER',SCAN_LAUNCH_STAGING_SESSION);
    const renderer=await mountPanel({objectId:'OBJ_1',sessionId:null,scanLaunchReady:true});
    await act(async()=>{await settleReact();(globalThis as any).__runPanelTimeouts();await settleReact();});
    expect(starts()).toBe(0);
    unmount(renderer);
  });
});

/* Issue #45, gate 1: a Continue scan is tied to the saved scan by known tags before any depth of the new session is used. */
describe('tag-locked Continue (issue #45)',()=>{
  /* The mocked pose puts a tag at WebXR (x, 2, 3 + z), i.e. project (x, -(3 + z), 2) metres; the saved layout below is that same placement, so B = A. */
  const TAGS=[{tagId:0,x:1,z:0},{tagId:1,x:2.5,z:0.4},{tagId:2,x:1.6,z:-1.1}];
  const projectMm=(tag:{x:number;z:number}):[number,number,number]=>[tag.x*1000,-(3+tag.z)*1000,2000];
  const node=(tag:typeof TAGS[number],confirmed=true)=>{const [x,y,z]=projectMm(tag);const [mx,my,mz]=projectMm(TAGS[0]);return{node_id:tag.tagId,tag_id:tag.tagId,x_mm:x-mx,y_mm:y-my,z_mm:z-mz,uncertainty_mm:3,confirmed,observations:9,views:4};};
  const artifact=(confirmedCount=3)=>({schema:1,kind:'tag-positions',frame:'sensor_z_up',scan_frame_id:BASE_ID,created_at_ms:1,geometry_computed_at_ms:null,
    main_scan_position_mm:projectMm(TAGS[0]),weakest_main_triangle_height_mm:400,nodes:TAGS.map((tag,index)=>node(tag,index<confirmedCount))} as any);
  const props=(extra:Record<string,unknown>={})=>({objectId:'OBJ_A',sessionId:'SESSION_A',scanIntent:'continue' as const,baseScanFrameId:BASE_ID,getRelockBase:()=>artifact(),...extra});
  const hint=(renderer:ReactTestRenderer,kind:string)=>renderer.root.findAll((n)=>n.props['data-relock']===kind);
  const observe=async(runtime:any,worker:any,tag:typeof TAGS[number],count:number,firstIndex:number,at?:{x:number;z:number})=>{
    for(let i=0;i<count;i+=1){runtime.emitTagFrame(tagFrame(firstIndex+i,[(firstIndex+i)*0.3,0,0]));await act(async()=>{emitTagDetection(worker,{tagId:tag.tagId,positionX:(at??tag).x,positionZ:(at??tag).z});await settleReact();});}
  };
  async function startContinue(extra:Record<string,unknown>={}){
    const renderer=await mountPanel(props(extra));const runtime=panelHarness.runtimes[0];await startGeometry(runtime);
    await act(async()=>{await settleReact();});const worker=panelHarness.workers[0];return{renderer,runtime,worker};
  }

  it('closes the depth gate when a Continue starts, asks for 3 known tags, and opens it only when the third is found',async()=>{
    const{renderer,runtime,worker}=await startContinue();
    expect(runtime.depthGate).toBe(true);
    expect(textOf(hint(renderer,'searching')[0])).toBe(translate('en','xr.relock.find',{needed:3,found:0}));
    await observe(runtime,worker,TAGS[0],4,1);
    expect(textOf(hint(renderer,'searching')[0])).toBe(translate('en','xr.relock.find',{needed:3,found:1}));
    expect(runtime.depthGate).toBe(true);
    await observe(runtime,worker,TAGS[0],6,10);                                    /* the same tag again is still one tag */
    expect(textOf(hint(renderer,'searching')[0])).toBe(translate('en','xr.relock.find',{needed:3,found:1}));
    await observe(runtime,worker,TAGS[1],4,20);
    expect(textOf(hint(renderer,'searching')[0])).toBe(translate('en','xr.relock.find',{needed:3,found:2}));
    expect(runtime.depthGate).toBe(true);
    await observe(runtime,worker,TAGS[2],4,30);
    expect(runtime.depthGate).toBe(false);
    expect(hint(renderer,'searching')).toHaveLength(0);
    expect(textOf(hint(renderer,'aligned')[0])).toBe(translate('en','xr.relock.aligned'));
    unmount(renderer);
  });

  it('keeps the camera view free of new buttons: the lock is a hint line and a status, nothing to press',async()=>{
    const{renderer}=await startContinue();
    const labels=renderer.root.findAllByType('button').map((button)=>textOf(button));
    expect(labels.some((label)=>/relock|lock|find tags|guided/i.test(label))).toBe(false);
    unmount(renderer);
  });

  it('does nothing for a Fresh scan, and for a saved scan with fewer than 3 confirmed tags (the Continue then works as before)',async()=>{
    let started=await startContinue({scanIntent:'fresh'});
    expect(started.runtime.depthGateCalls.includes(true)).toBe(false);
    expect(hint(started.renderer,'searching')).toHaveLength(0);
    unmount(started.renderer);
    panelHarness.runtimes=[];panelHarness.workers=[];
    started=await startContinue({getRelockBase:()=>artifact(2)});
    expect(started.runtime.depthGateCalls.includes(true)).toBe(false);
    expect(hint(started.renderer,'searching')).toHaveLength(0);
    unmount(started.renderer);
    panelHarness.runtimes=[];panelHarness.workers=[];
    started=await startContinue({getRelockBase:()=>({...artifact(),scan_frame_id:'scan-crc32:bbbbbbbb'})});   /* tags of another scan anchor nothing */
    expect(started.runtime.depthGateCalls.includes(true)).toBe(false);
    unmount(started.renderer);
  });

  it('voids the lock on a reference-space reset, drops what was seen before it, and needs the tags again',async()=>{
    const{renderer,runtime,worker}=await startContinue();
    for(let k=0;k<3;k+=1)await observe(runtime,worker,TAGS[k],4,1+k*10);
    expect(runtime.depthGate).toBe(false);
    await act(async()=>{runtime.setPhase('running',{originReset:true});await settleReact();});
    expect(runtime.depthGate).toBe(true);
    expect(textOf(hint(renderer,'searching')[0])).toBe(translate('en','xr.relock.find',{needed:3,found:0}));
    await act(async()=>{runtime.setPhase('running',{originReset:false});await settleReact();});
    for(let k=0;k<3;k+=1)await observe(runtime,worker,TAGS[k],4,100+k*10);
    expect(runtime.depthGate).toBe(false);
    unmount(renderer);
  });

  it('ends the lock when a tag it relied on jumps (the frame shifted without a reset event)',async()=>{
    const{renderer,runtime,worker}=await startContinue();
    for(let k=0;k<3;k+=1)await observe(runtime,worker,TAGS[k],4,1+k*10);
    expect(runtime.depthGate).toBe(false);
    await observe(runtime,worker,TAGS[2],5,200,{x:9,z:2});                          /* the same tag, 8 m away */
    expect(runtime.depthGate).toBe(true);
    expect(hint(renderer,'searching').length).toBeGreaterThan(0);
    unmount(renderer);
  });

  it('hands nothing over when the tags were never found, says so, and leaves the saved scan alone; a locked Continue hands its scan over',async()=>{
    const ready=vi.fn();
    let started=await startContinue({onPointCloudReady:ready});
    expect(ready.mock.calls).toEqual([[null]]);                                     /* only the startup notice */
    await act(async()=>{started.runtime.setPhase('ended',{pointCount:50});await settleReact();});
    expect(ready.mock.calls).toEqual([[null]]);
    expect(hint(started.renderer,'never-locked')).toHaveLength(1);
    expect(textOf(hint(started.renderer,'never-locked')[0])).toBe(translate('en','xr.relock.never_locked'));
    expect(started.runtime.depthGate).toBe(false);                                  /* the gate never outlives the session */
    unmount(started.renderer);

    panelHarness.runtimes=[];panelHarness.workers=[];
    const lockedReady=vi.fn();
    started=await startContinue({onPointCloudReady:lockedReady});
    for(let k=0;k<3;k+=1)await observe(started.runtime,started.worker,TAGS[k],4,1+k*10);
    await act(async()=>{started.runtime.setPhase('ended',{pointCount:50});await settleReact();});
    expect(lockedReady).toHaveBeenCalledTimes(2);
    expect(lockedReady.mock.calls[1][0]).not.toBeNull();
    expect(hint(started.renderer,'never-locked')).toHaveLength(0);
    unmount(started.renderer);
  });

  describe('the saved scan drawn over the camera once locked (gate 2)',()=>{
    /* A's measured points (project metres). With B = A the lock is the identity, so project (x, y, z) must be drawn at WebXR (x, z, -y). */
    const REFERENCE=Float32Array.from([1,-3,2, 2.5,-3.4,2, 1.6,-1.9,2.5]);
    const lockedPanel=async(extra:Record<string,unknown>={})=>{
      const started=await startContinue({getReferencePoints:()=>REFERENCE,...extra});
      for(let k=0;k<3;k+=1)await observe(started.runtime,started.worker,TAGS[k],4,1+k*10);
      return started;
    };

    it('shows nothing before the lock, then hands the runtime A\'s points in this session\'s frame',async()=>{
      const{renderer,runtime,worker}=await startContinue({getReferencePoints:()=>REFERENCE});
      expect(runtime.referenceOverlay).toBeNull();
      await observe(runtime,worker,TAGS[0],4,1);await observe(runtime,worker,TAGS[1],4,20);
      expect(runtime.referenceOverlay).toBeNull();                                  /* two tags: still searching */
      await observe(runtime,worker,TAGS[2],4,30);
      const overlay=runtime.referenceOverlay;
      expect(overlay).not.toBeNull();
      expect(overlay.pointCount).toBe(3);
      const expected=[1,2,3, 2.5,2,3.4, 1.6,2.5,1.9];
      for(let i=0;i<expected.length;i+=1)expect(overlay.positions[i]).toBeCloseTo(expected[i],3);
      expect(Array.from(overlay.confidence)).toHaveLength(3);
      unmount(renderer);
    });

    it('removes it when the lock goes: a reset of the reference space, a jumped tag, the end of the session, the panel going away',async()=>{
      let started=await lockedPanel();
      await act(async()=>{started.runtime.setPhase('running',{originReset:true});await settleReact();});
      expect(started.runtime.referenceOverlay).toBeNull();
      unmount(started.renderer);

      panelHarness.runtimes=[];panelHarness.workers=[];
      started=await lockedPanel();
      expect(started.runtime.referenceOverlay).not.toBeNull();
      await observe(started.runtime,started.worker,TAGS[2],5,200,{x:9,z:2});
      expect(started.runtime.referenceOverlay).toBeNull();
      unmount(started.renderer);

      panelHarness.runtimes=[];panelHarness.workers=[];
      started=await lockedPanel();
      await act(async()=>{started.runtime.setPhase('ended',{pointCount:50});await settleReact();});
      expect(started.runtime.referenceOverlay).toBeNull();
      unmount(started.renderer);

      panelHarness.runtimes=[];panelHarness.workers=[];
      started=await lockedPanel();
      expect(started.runtime.referenceOverlay).not.toBeNull();
      unmount(started.renderer);
      expect(started.runtime.referenceOverlay).toBeNull();
    });

    it('draws nothing when the parent has no points to give, and nothing for a Fresh scan',async()=>{
      let started=await lockedPanel({getReferencePoints:()=>null});
      expect(started.runtime.referenceOverlay).toBeNull();
      unmount(started.renderer);
      panelHarness.runtimes=[];panelHarness.workers=[];
      started=await startContinue({scanIntent:'fresh',getReferencePoints:()=>REFERENCE});
      for(let k=0;k<3;k+=1)await observe(started.runtime,started.worker,TAGS[k],4,1+k*10);
      expect(started.runtime.overlayCalls.some((overlay:any)=>overlay!==null)).toBe(false);
      unmount(started.renderer);
    });
  });

  it('opens the gate again when the panel goes away',async()=>{
    const{renderer,runtime}=await startContinue();
    expect(runtime.depthGate).toBe(true);
    unmount(renderer);
    expect(runtime.depthGate).toBe(false);
  });
});
