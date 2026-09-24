import { useEffect, useMemo, useRef, useState } from 'react';

import { clientDiagnostics } from '../data/clientDiagnostics';
import { StructuralGuideOverlay } from './StructuralGuideOverlay';
import { BleRoughPlacementHud } from './BleRoughPlacementHud';
import { BleRoughPlacement } from '../xr/bleRoughPlacement';
import { bleRssiSupport, describeBleRssiSupport, WebBluetoothRssiSource } from '../xr/bleRssiSource';
import { describeAiPrior } from '../xr/aiPriorUi';
import {
  getCertifiedGuideRegistrySummary,
  subscribeCertifiedGuideRegistry,
} from '../xr/certifiedCornerRegistry';
import { liveComputeScheduler } from '../xr/liveComputeScheduler';
import {
  manualPreviewLabel,
  previewManualPlacementFromStableSurface,
  type ManualPlacementPreview,
} from '../xr/manualPlacementPreview';
import { optionalMonocularPrior } from '../xr/monocularPriorRuntime';
import { emptyXrPerformanceSnapshot } from '../xr/xrPerformanceProfiler';
import {
  canAttemptImmersiveAr,
  launchImmersiveArFromUserGesture,
} from '../xr/xrLaunchPolicy';
import {
  probeWebXrCapabilities,
  WebXrRuntime,
  type ScanPointCloudSnapshot,
  type WebXrCapabilities,
  type WebXrPoseSnapshot,
  type WebXrRuntimeState,
  type XrRuntimePhase,
} from '../xr/webxrRuntime';

const AI_PRIOR_STORAGE_KEY = 'xr.optionalMonocularPrior.enabled';

export type ScanStartIntent = 'fresh' | 'continue';

const emptyRuntimeState: WebXrRuntimeState = {
  phase:'idle',referenceSpace:null,domOverlay:false,environmentBlendMode:null,frameCount:0,tracking:false,pose:null,
  scanPaused:false,wakeLockActive:false,wakeLockError:null,depthAvailable:false,depthUsage:null,depthDataFormat:null,depthType:null,
  depthWidth:null,depthHeight:null,depthFrameCount:0,sampledDepthFrames:0,pointCount:0,retainedPointCount:0,pointCloudVersion:0,
  keyframeCount:0,polishFrameCount:0,motionRejectedFrames:0,motionState:null,motionQuality:0,linearSpeedMetersPerSecond:0,
  angularSpeedRadiansPerSecond:0,linearAccelerationMetersPerSecond2:0,coverageGuideActive:false,coverageGuideRenderPoints:0,
  coverageVisibleCells:0,coverageReadyCells:0,coverageTotalCells:0,coverageTargetIndependentViews:3,coverageAverageConfidence:0,
  coverageWeakPoints:0,coverageImprovingPoints:0,coverageStablePoints:0,coverageGuideError:null,manualPointCount:0,manualEdgeCount:0,
  manualPointError:null,performance:emptyXrPerformanceSnapshot,lightingEstimateAvailable:false,primaryLightIntensity:null,
  rgbGuidanceAvailable:false,rgbGuidanceWidth:null,rgbGuidanceHeight:null,visualEvidenceFrames:0,monocularPriorStatus:'disabled',depthError:null,error:null,
};

interface XrRuntimePanelProps {
  savedScanAvailable?: boolean;
  savedScanBusy?: boolean;
  onScanIntent?: (intent: ScanStartIntent) => boolean;
  onDeleteSavedScan?: () => void;
  onSessionActiveChange?: (active:boolean)=>void;
  onPointCloudReady?: (snapshot:ScanPointCloudSnapshot|null)=>void;
  onCapabilitiesChange?: (capabilities:WebXrCapabilities)=>void;
}
interface ManualPointerState {
  pointerId:number;startX:number;startY:number;startU:number;startV:number;latestX:number;latestY:number;
  dragging:boolean;movePointId:number|null;timer:number|null;captureActive:boolean;
}
type ManualCursorPhase='scan-more'|'edge'|'corner'|'dragging'|'confirmed';
interface ManualCursorState {u:number;v:number;phase:ManualCursorPhase;preview:ManualPlacementPreview|null;distanceMeters:number|null;}
const LONG_PRESS_MS=380;const PREVIEW_UPDATE_MS=110;const TAP_SLOP_PX=16;

function readAiPriorPreference():boolean{
  if(typeof window==='undefined')return false;
  try{return window.localStorage.getItem(AI_PRIOR_STORAGE_KEY)==='1';}catch{return false;}
}
function persistAiPriorPreference(enabled:boolean):void{
  if(typeof window==='undefined')return;
  try{window.localStorage.setItem(AI_PRIOR_STORAGE_KEY,enabled?'1':'0');}catch{/* optional preference only */}
}
function phaseLabel(state:WebXrRuntimeState):string{
  if(state.phase==='starting')return'STARTING';if(state.phase==='stopping')return'FINISHING';if(state.phase==='interrupted')return'SCAN INTERRUPTED';
  if(state.phase==='ended')return state.pointCount>0?'SCAN READY':'NO STABLE SURFACE';if(state.phase==='error')return'ERROR';if(state.phase!=='running')return'READY';
  if(state.scanPaused)return'PAUSED';if(!state.tracking)return'FINDING TRACKING';if(!state.depthAvailable)return'DEPTH UNAVAILABLE';if(state.motionState==='too-fast')return'MOVE SLOWER';
  if(state.motionState==='stationary'||state.motionState==='redundant')return'MOVE TO A NEW VIEW';if(!state.coverageGuideActive)return'BUILDING SURFACE';return'SCANNING';
}
function qualityClass(state:WebXrRuntimeState):string{if(state.phase==='error'||state.phase==='interrupted')return'bad';if(state.phase==='running'&&state.motionState==='too-fast')return'warning';if(state.phase==='running'||state.phase==='ended')return'good';return'';}
function formatPercent(value:number):string{return`${Math.round(Math.max(0,Math.min(1,value))*100)}%`;}
function formatMotion(state:WebXrRuntimeState):string{
  if(state.scanPaused)return'PAUSED';switch(state.motionState){case'keyframe':return'NEW VIEW ✓';case'polish':return'POLISHING';case'stationary':return'MOVE PHONE';case'redundant':return'NEW ANGLE';case'too-fast':return'TOO FAST';case'initializing':return'INITIALIZING';default:return'--';}
}
function normalizedPointer(event:React.PointerEvent<HTMLElement>):readonly[number,number]{const rect=event.currentTarget.getBoundingClientRect();const width=Math.max(1,rect.width),height=Math.max(1,rect.height);return[Math.max(0,Math.min(1,(event.clientX-rect.left)/width)),Math.max(0,Math.min(1,(event.clientY-rect.top)/height))];}
function previewPhase(preview:ManualPlacementPreview):ManualCursorPhase{return!preview.resolution.accepted||preview.resolution.position===null?'scan-more':preview.resolution.mode==='corner'?'corner':'edge';}
function previewDistanceMeters(preview:ManualPlacementPreview,state:WebXrRuntimeState):number|null{const target=preview.resolution.position,pose=state.pose;if(target===null||pose===null)return null;return Math.hypot(target[0]-pose.position[0],target[1]+pose.position[2],target[2]-pose.position[1]);}
function cursorLabel(cursor:ManualCursorState,count:number):string{if(cursor.phase==='dragging'&&cursor.preview===null)return'MOVE RETICLE';if(cursor.phase==='confirmed')return`PIN ${count} ✓`;if(cursor.phase==='scan-more'||cursor.preview===null)return'SCAN MORE';const confidence=Math.round(cursor.preview.resolution.confidence*100);const distance=cursor.distanceMeters===null?'':` · ${cursor.distanceMeters.toFixed(2)} m`;return`${manualPreviewLabel(cursor.preview)} ${confidence}%${distance}`;}

export function XrRuntimePanel({
  savedScanAvailable=false,
  savedScanBusy=false,
  onScanIntent,
  onDeleteSavedScan,
  onSessionActiveChange,
  onPointCloudReady,
  onCapabilitiesChange,
}:XrRuntimePanelProps){
  const runtime=useMemo(()=>new WebXrRuntime(),[]);const[capabilities,setCapabilities]=useState<WebXrCapabilities|null>(null);const[runtimeState,setRuntimeState]=useState<WebXrRuntimeState>(emptyRuntimeState);const[manualMode,setManualMode]=useState(false);const[aiPriorEnabled,setAiPriorEnabled]=useState(readAiPriorPreference);const[computePreparing,setComputePreparing]=useState(false);const[activeScanIntent,setActiveScanIntent]=useState<ScanStartIntent>('fresh');const[,setAiPriorRevision]=useState(0);const[,setCertifiedGeometryRevision]=useState(0);
  const[manualCursor,setManualCursor]=useState<ManualCursorState>({u:.5,v:.5,phase:'scan-more',preview:null,distanceMeters:null});const manualPointer=useRef<ManualPointerState|null>(null);const lastPreviewUpdateMs=useRef(Number.NEGATIVE_INFINITY);const previousManualCount=useRef(0);const previousRuntimePhase=useRef<XrRuntimePhase>('idle');
  const onSessionActiveChangeRef=useRef(onSessionActiveChange),onPointCloudReadyRef=useRef(onPointCloudReady),onCapabilitiesChangeRef=useRef(onCapabilitiesChange);
  useEffect(()=>{onSessionActiveChangeRef.current=onSessionActiveChange;},[onSessionActiveChange]);useEffect(()=>{onPointCloudReadyRef.current=onPointCloudReady;},[onPointCloudReady]);useEffect(()=>{onCapabilitiesChangeRef.current=onCapabilitiesChange;},[onCapabilitiesChange]);
  useEffect(()=>optionalMonocularPrior.subscribe(()=>setAiPriorRevision((revision)=>revision+1)),[]);
  useEffect(()=>subscribeCertifiedGuideRegistry(()=>setCertifiedGeometryRevision((revision)=>revision+1)),[]);
  useEffect(()=>{optionalMonocularPrior.setEnabled(aiPriorEnabled);},[aiPriorEnabled]);
  useEffect(()=>{const unsubscribe=runtime.subscribe(setRuntimeState);let cancelled=false;void probeWebXrCapabilities().then((result)=>{clientDiagnostics.info('XR_CAPABILITIES',result);void clientDiagnostics.flush();if(!cancelled){setCapabilities(result);onCapabilitiesChangeRef.current?.(result);}});return()=>{cancelled=true;const pointer=manualPointer.current;if(pointer?.timer!=null)window.clearTimeout(pointer.timer);manualPointer.current=null;unsubscribe();void runtime.dispose();};},[runtime]);
  useEffect(()=>{const active=runtimeState.phase==='starting'||runtimeState.phase==='running'||runtimeState.phase==='stopping';onSessionActiveChangeRef.current?.(active);if(!active){setManualMode(false);manualPointer.current=null;}},[runtimeState.phase]);
  useEffect(()=>{const previous=previousRuntimePhase.current;previousRuntimePhase.current=runtimeState.phase;if(runtimeState.phase==='starting'&&previous!=='starting')onPointCloudReadyRef.current?.(null);else if(runtimeState.phase==='ended'&&previous!=='ended'&&runtimeState.pointCount>0)onPointCloudReadyRef.current?.(runtime.getPointCloudSnapshot());},[runtime,runtimeState.phase,runtimeState.pointCount]);
  useEffect(()=>{if(runtimeState.manualPointCount>previousManualCount.current)setManualCursor((current)=>({...current,phase:'confirmed'}));previousManualCount.current=runtimeState.manualPointCount;},[runtimeState.manualPointCount]);
  useEffect(()=>{if(runtimeState.manualPointError!==null)setManualCursor((current)=>({...current,phase:'scan-more'}));},[runtimeState.manualPointError]);
  /* MAX-146: optional BLE RSSI node hints. Samples are only taken with a live XR pose and reset per session (new reference frame). */
  const bleNavigator=typeof navigator==='undefined'?undefined:(navigator as unknown as Parameters<typeof bleRssiSupport>[0]);
  const bleSupport=useMemo(()=>bleRssiSupport(bleNavigator,typeof window!=='undefined'&&window.isSecureContext),[bleNavigator]);
  const blePlacement=useMemo(()=>new BleRoughPlacement(),[]);const bleSource=useMemo(()=>new WebBluetoothRssiSource(bleNavigator),[bleNavigator]);
  const[bleActive,setBleActive]=useState(false);const[bleTick,setBleTick]=useState(0);const posePointer=useRef<WebXrPoseSnapshot|null>(null);
  useEffect(()=>{posePointer.current=runtimeState.phase==='running'&&runtimeState.tracking?runtimeState.pose:null;},[runtimeState.phase,runtimeState.tracking,runtimeState.pose]);
  useEffect(()=>()=>bleSource.stop(),[bleSource]);
  useEffect(()=>{if(runtimeState.phase==='starting')blePlacement.reset();else if((runtimeState.phase==='ended'||runtimeState.phase==='interrupted')&&bleSource.active){clientDiagnostics.info('XR_BLE_RSSI_DIAGNOSTICS',blePlacement.diagnostics(Date.now()));void clientDiagnostics.flush();}},[runtimeState.phase,blePlacement,bleSource]);
  useEffect(()=>{if(!bleActive||runtimeState.phase!=='running')return;const id=window.setInterval(()=>setBleTick((tick)=>tick+1),1000);return()=>window.clearInterval(id);},[bleActive,runtimeState.phase]);
  const bleEstimates=useMemo(()=>(bleActive&&bleTick>=0?blePlacement.estimates(Date.now()):[]),[blePlacement,bleActive,bleTick]);
  const toggleBle=async():Promise<void>=>{
    if(bleSource.active){bleSource.stop();setBleActive(false);clientDiagnostics.info('XR_BLE_RSSI',{enabled:false});return;}
    const started=await bleSource.start((advert)=>{const pose=posePointer.current;if(pose===null)return;blePlacement.addSample({...advert,phonePosition:pose.position,timestampMs:Date.now()});});
    setBleActive(started);clientDiagnostics.info('XR_BLE_RSSI',{enabled:started,support:bleSupport});void clientDiagnostics.flush();
  };

  const running=runtimeState.phase==='running',busy=computePreparing||runtimeState.phase==='starting'||runtimeState.phase==='stopping';const canStart=canAttemptImmersiveAr(capabilities)&&!running&&!busy;const readyVisible=`${runtimeState.coverageReadyCells.toLocaleString()} / ${runtimeState.coverageVisibleCells.toLocaleString()}`;
  const computeScheduler=liveComputeScheduler.snapshot();const computeMaximum=computeScheduler.maximumBudget;const computeEffective=computeScheduler.effectiveBudget;const computeLabel=computeMaximum===null?'W?/?':`W${computeEffective??0}/${computeMaximum}`;
  const certifiedGeometry=getCertifiedGuideRegistrySummary();
  /*
   * Manual marking must be ENTERABLE before a worker happens to certify a target.
   * The placement resolver still rejects release unless EDGE/CORNER LOCK is
   * physically certified by either the global registry or the local robust
   * stable-surface solver, so exposing the button does not weaken geometry safety.
   */
  const showManualToggle=running;
  const aiPriorUi=describeAiPrior({
    enabled:aiPriorEnabled,
    phase:runtimeState.phase,
    providerStatus:optionalMonocularPrior.status,
    hasStagedPostScanFrame:optionalMonocularPrior.hasStagedPostScanFrame,
    busy:optionalMonocularPrior.isBusy,
    calibrationConfidence:optionalMonocularPrior.calibrationConfidence,
    calibrationModel:optionalMonocularPrior.calibrationModel,
    error:optionalMonocularPrior.error,
  });
  const toggleAiPrior=():void=>{setAiPriorEnabled((current)=>{const next=!current;persistAiPriorPreference(next);clientDiagnostics.info('XR_OPTIONAL_AI_PRIOR',{enabled:next,status:next?optionalMonocularPrior.status:'disabled'});void clientDiagnostics.flush();return next;});};
  const clearManualTimer=():void=>{const pointer=manualPointer.current;if(pointer?.timer!=null){window.clearTimeout(pointer.timer);pointer.timer=null;}};
  const computeManualPreview=(u:number,v:number):ManualPlacementPreview=>previewManualPlacementFromStableSurface(u,v,runtime.getStructuralGuideSnapshot(),(point)=>runtime.projectProjectPointToNormalized(point));
  const refreshManualPreview=(u:number,v:number,force=false):ManualPlacementPreview|null=>{const now=performance.now();if(!force&&now-lastPreviewUpdateMs.current<PREVIEW_UPDATE_MS){setManualCursor((current)=>({...current,u,v}));return null;}lastPreviewUpdateMs.current=now;const preview=computeManualPreview(u,v);setManualCursor({u,v,phase:previewPhase(preview),preview,distanceMeters:previewDistanceMeters(preview,runtimeState)});return preview;};

  const handleManualPointerDown=(event:React.PointerEvent<HTMLDivElement>):void=>{
    if(!running||!manualMode)return;event.preventDefault();let captureActive=false;try{event.currentTarget.setPointerCapture(event.pointerId);captureActive=true;}catch(error){clientDiagnostics.warn('XR_MANUAL_POINTER_CAPTURE_UNAVAILABLE',error);}
    const movePointId=runtime.getLastManualPointId();const[startU,startV]=normalizedPointer(event);refreshManualPreview(startU,startV,true);const pointerId=event.pointerId;
    const state:ManualPointerState={pointerId,startX:event.clientX,startY:event.clientY,startU,startV,latestX:event.clientX,latestY:event.clientY,dragging:false,movePointId,timer:null,captureActive};
    state.timer=window.setTimeout(()=>{const current=manualPointer.current;if(current===null||current.pointerId!==pointerId||current.movePointId===null)return;current.dragging=true;const preview=refreshManualPreview(current.startU,current.startV,true);setManualCursor((cursor)=>({...cursor,u:current.startU,v:current.startV,phase:preview?previewPhase(preview):'dragging'}));clientDiagnostics.info('XR_MANUAL_DRAG_BEGIN',{id:current.movePointId});},LONG_PRESS_MS);manualPointer.current=state;
  };
  const handleManualPointerMove=(event:React.PointerEvent<HTMLDivElement>):void=>{const pointer=manualPointer.current;if(pointer===null||pointer.pointerId!==event.pointerId)return;pointer.latestX=event.clientX;pointer.latestY=event.clientY;const[u,v]=normalizedPointer(event);refreshManualPreview(u,v);};
  const handleManualPointerUp=(event:React.PointerEvent<HTMLDivElement>):void=>{
    const pointer=manualPointer.current;if(pointer===null||pointer.pointerId!==event.pointerId)return;clearManualTimer();const moved=Math.hypot(event.clientX-pointer.startX,event.clientY-pointer.startY);const[u,v]=normalizedPointer(event);const preview=refreshManualPreview(u,v,true)??computeManualPreview(u,v);const resolved=preview.resolution;
    if(resolved.accepted&&resolved.position!==null){try{
      if(pointer.dragging&&pointer.movePointId!==null){runtime.queueManualPointAtNormalized(u,v,pointer.movePointId);clientDiagnostics.info('XR_MANUAL_POINTER_MOVE_COMMIT',{id:pointer.movePointId,u:Number(u.toFixed(3)),v:Number(v.toFixed(3)),mode:resolved.mode});}
      else{const stored=runtime.addSuggestedManualPoint(resolved.position);if(stored!==null){setManualCursor({u,v,phase:'confirmed',preview,distanceMeters:previewDistanceMeters(preview,runtimeState)});clientDiagnostics.info(moved<=TAP_SLOP_PX?'XR_MANUAL_STABLE_TAP':'XR_MANUAL_STABLE_DRAG_RELEASE',{id:stored.point.id,mode:resolved.mode,confidence:Number(resolved.confidence.toFixed(3)),distanceMeters:previewDistanceMeters(preview,runtimeState),movedPx:Math.round(moved)});}}
    }catch(error){setManualCursor({u,v,phase:'scan-more',preview,distanceMeters:null});clientDiagnostics.warn('XR_MANUAL_PREVIEW_COMMIT_REJECTED',error);}}
    else clientDiagnostics.info('XR_MANUAL_PREVIEW_BLOCKED',{u:Number(u.toFixed(3)),v:Number(v.toFixed(3)),reason:resolved.message});
    void clientDiagnostics.flush();if(pointer.captureActive){try{event.currentTarget.releasePointerCapture(event.pointerId);}catch{/*released*/}}manualPointer.current=null;
  };
  const handleManualPointerCancel=(event:React.PointerEvent<HTMLDivElement>):void=>{const pointer=manualPointer.current;clearManualTimer();if(pointer?.captureActive){try{event.currentTarget.releasePointerCapture(event.pointerId);}catch{/*released*/}}manualPointer.current=null;setManualCursor((current)=>({...current,phase:'scan-more'}));clientDiagnostics.info('XR_MANUAL_POINTER_CANCEL');};
  const toggleManualMode=():void=>{setManualMode((value)=>{const next=!value;if(!next){clearManualTimer();manualPointer.current=null;setManualCursor((current)=>({...current,phase:'scan-more',preview:null,distanceMeters:null}));}else refreshManualPreview(.5,.5,true);clientDiagnostics.info('XR_MANUAL_MODE',{enabled:next,points:runtimeState.manualPointCount});void clientDiagnostics.flush();return next;});};
  const startPreparedScan=async(intent:ScanStartIntent):Promise<void>=>{
    if(computePreparing||!canStart)return;
    if(onScanIntent?.(intent)===false)return;
    setActiveScanIntent(intent);
    setComputePreparing(true);
    try{
      await launchImmersiveArFromUserGesture({beginScan:()=>liveComputeScheduler.beginScan(),startRuntime:()=>runtime.start(),prepareCompute:()=>liveComputeScheduler.prepare(),onPrepared:(prepared)=>{clientDiagnostics.info('XR_COMPUTE_BUDGET_PREPARED',{maximumWorkers:prepared.maximumBudget,effectiveWorkers:prepared.effectiveBudget,pressure:prepared.pressure,degraded:prepared.degraded,scanIntent:intent});void clientDiagnostics.flush();}});
    }finally{setComputePreparing(false);}
  };

  const continuationSampleReady=runtimeState.pointCount>=120;

  return <>
    <section className="xr-runtime-card xr-runtime-card-compact" aria-label="3D scanner">
      <div className="xr-runtime-heading"><div><div className="panel-title">3D SCANNER</div><strong>ARCore depth · RGB-guided robust fusion</strong></div><span className={`xr-phase ${qualityClass(runtimeState)}`}>{phaseLabel(runtimeState)}</span></div>
      <div className="xr-compact-status"><span className={capabilities?.immersiveAr?'good-text':'bad-text'}>WebXR {capabilities?.immersiveAr?'✓':capabilities===null?'checking…':'—'}</span><span className={runtimeState.depthAvailable?'good-text':''}>Depth {runtimeState.depthAvailable?'✓':runtimeState.phase==='idle'?'on start':'—'}</span><span className={runtimeState.rgbGuidanceAvailable?'good-text':''}>RGB {runtimeState.rgbGuidanceAvailable?'✓':capabilities?.rawXrRgbApi?'on start':'—'}</span><span className={aiPriorUi.good?'good-text':''}>AI prior {aiPriorUi.shortStatus}</span><span className={runtimeState.wakeLockActive?'good-text':''}>Screen {runtimeState.wakeLockActive?'awake ✓':capabilities?.wakeLockApi?'wake-ready':'wake-lock —'}</span></div>
      <div className="xr-runtime-controls xr-runtime-controls-compact">
        {savedScanAvailable ? <>
          <button type="button" className="xr-start-button" disabled={!canStart||savedScanBusy} onClick={()=>void startPreparedScan('continue')}>{computePreparing?'Starting AR…':'Continue scanning'}</button>
          <button type="button" className="xr-undo-button" disabled={!canStart||savedScanBusy} onClick={()=>void startPreparedScan('fresh')}>Scan again</button>
          {onDeleteSavedScan!==undefined&&<button type="button" className="xr-undo-button" disabled={busy||savedScanBusy} onClick={onDeleteSavedScan}>Delete saved scan</button>}
          <div className="xr-finish-explainer">Continue: begin on a clearly recognizable part of the saved scan and collect several overlapping views before expanding into new space. Scan again starts a replacement scan, but the old SD artifact is kept until the new result saves successfully.</div>
        </> : <>
          <button type="button" className="xr-start-button" disabled={!canStart||savedScanBusy} onClick={()=>void startPreparedScan('fresh')}>{computePreparing?'Starting AR…':runtimeState.phase==='interrupted'?'Discard partial & start again':'Start 3D scan'}</button>
        </>}
        <button type="button" className={aiPriorEnabled?'xr-undo-button active':'xr-undo-button'} aria-pressed={aiPriorEnabled} onClick={toggleAiPrior} title={aiPriorUi.explanation}>{aiPriorEnabled?'AI geometry check: ON · after Finish':'AI geometry check: OFF'}</button>
        {bleSupport==='supported'?<button type="button" className={bleActive?'xr-undo-button active':'xr-undo-button'} aria-pressed={bleActive} onClick={()=>void toggleBle()} title="Rough node hints from BLE signal strength; never used as sensor geometry">{bleActive?'BLE node hints: ON':'BLE node hints: OFF'}</button>:<div className="xr-finish-explainer">{describeBleRssiSupport(bleSupport)}</div>}
        <div className="xr-finish-explainer">AI is optional and defaults OFF. When enabled, live XR only stages RGB + physical anchors; ONNX inference starts after Finish &amp; save.</div>
      </div>
      {aiPriorEnabled&&optionalMonocularPrior.status==='unavailable'&&<div className="xr-runtime-warning" role="status">Optional AI prior is unavailable on this browser/GPU. Physical ARCore depth and RGB structural validation continue normally.</div>}
      {aiPriorEnabled&&optionalMonocularPrior.status==='error'&&<div className="xr-runtime-warning" role="status">Optional AI prior failed: {optionalMonocularPrior.error??'unknown error'}. Physical ARCore depth remains authoritative.</div>}
      {runtimeState.phase==='interrupted'&&<div className="xr-runtime-warning" role="status">XR session ended unexpectedly. Partial geometry is retained in memory but was not marked READY because a new session may use a different reference frame.</div>}
      {runtimeState.depthError!==null&&<div className="xr-runtime-warning" role="status">{runtimeState.depthError}</div>}{runtimeState.error!==null&&<div className="xr-runtime-error" role="alert">{runtimeState.error}</div>}
      <details className="xr-technical-details"><summary>Technical details</summary><dl className="xr-runtime-values">
        <div><dt>Depth type</dt><dd>{runtimeState.depthType??'--'}</dd></div><div><dt>Depth usage</dt><dd>{runtimeState.depthUsage??'--'}</dd></div><div><dt>Depth format</dt><dd>{runtimeState.depthDataFormat??'--'}</dd></div><div><dt>Depth size</dt><dd>{runtimeState.depthWidth&&runtimeState.depthHeight?`${runtimeState.depthWidth} × ${runtimeState.depthHeight}`:'--'}</dd></div>
        <div><dt>Raw XR RGB API</dt><dd>{capabilities?.rawXrRgbApi?'AVAILABLE':'--'}</dd></div><div><dt>RGB guidance</dt><dd>{runtimeState.rgbGuidanceAvailable?`${runtimeState.rgbGuidanceWidth} × ${runtimeState.rgbGuidanceHeight}`:'--'}</dd></div><div><dt>Video inputs</dt><dd>{capabilities?.videoInputsEnumerated?(capabilities.videoInputCount??0):capabilities?.videoInputEnumeration?'not exposed':'--'}</dd></div><div><dt>Hardware depth sensor</dt><dd>{capabilities?.hardwareDepthSensor?.toUpperCase()??'UNKNOWN'} · not inferred from camera names</dd></div><div><dt>Visual evidence frames</dt><dd>{runtimeState.visualEvidenceFrames}</dd></div><div><dt>AI depth prior</dt><dd>{aiPriorUi.shortStatus}</dd></div><div><dt>AI calibration</dt><dd>{aiPriorUi.calibrationLabel}</dd></div>
        <div><dt>Stable points</dt><dd>{runtimeState.pointCount.toLocaleString()}</dd></div><div><dt>Retained hypotheses</dt><dd>{runtimeState.retainedPointCount.toLocaleString()}</dd></div><div><dt>Keyframes</dt><dd>{runtimeState.keyframeCount.toLocaleString()}</dd></div><div><dt>Polish frames</dt><dd>{runtimeState.polishFrameCount.toLocaleString()}</dd></div><div><dt>Motion rejects</dt><dd>{runtimeState.motionRejectedFrames.toLocaleString()}</dd></div>
        <div><dt>Ready / visible cells</dt><dd>{readyVisible}</dd></div><div><dt>Coverage confidence</dt><dd>{formatPercent(runtimeState.coverageAverageConfidence)}</dd></div><div><dt>Weak points</dt><dd>{runtimeState.coverageWeakPoints.toLocaleString()}</dd></div><div><dt>Improving points</dt><dd>{runtimeState.coverageImprovingPoints.toLocaleString()}</dd></div><div><dt>Stable guide points</dt><dd>{runtimeState.coverageStablePoints.toLocaleString()}</dd></div>
        <div><dt>Worker API</dt><dd>{capabilities?.workerApi?'AVAILABLE':'--'}</dd></div><div><dt>Shared memory</dt><dd>{capabilities?.sharedMemory?'AVAILABLE':'--'}</dd></div><div><dt>Wasm threads</dt><dd>{capabilities?.wasmThreads?'AVAILABLE':'--'}</dd></div><div><dt>WebGPU</dt><dd>{capabilities?.webGpu?'AVAILABLE':'--'}</dd></div><div><dt>Live CPU workers</dt><dd>{computeLabel} · {computeScheduler.pressure.toUpperCase()}{computeScheduler.degraded?' throttled':''}</dd></div><div><dt>Active heavy jobs</dt><dd>{computeScheduler.active} · S{computeScheduler.structural} / V{computeScheduler.visual}</dd></div><div><dt>Manual corners</dt><dd>{runtimeState.manualPointCount.toLocaleString()}</dd></div><div><dt>Confirmed edges</dt><dd>{runtimeState.manualEdgeCount.toLocaleString()}</dd></div><div><dt>Frame p95</dt><dd>{runtimeState.performance.frameDeltaP95Ms.toFixed(1)} ms</dd></div><div><dt>Fusion p50 / p95</dt><dd>{runtimeState.performance.fusionP50Ms.toFixed(1)} / {runtimeState.performance.fusionP95Ms.toFixed(1)} ms</dd></div><div><dt>Coverage p95</dt><dd>{runtimeState.performance.coverageP95Ms.toFixed(1)} ms</dd></div><div><dt>Long frames ≥33 / ≥50 ms</dt><dd>{runtimeState.performance.longFrames33Ms} / {runtimeState.performance.longFrames50Ms}</dd></div><div><dt>Live render pressure</dt><dd>{runtimeState.performance.pressure.toUpperCase()} · {runtimeState.performance.adaptiveCoverageIntervalMs} ms</dd></div>
        <div><dt>Linear speed</dt><dd>{runtimeState.linearSpeedMetersPerSecond.toFixed(2)} m/s</dd></div><div><dt>Angular speed</dt><dd>{runtimeState.angularSpeedRadiansPerSecond.toFixed(2)} rad/s</dd></div><div><dt>Acceleration</dt><dd>{runtimeState.linearAccelerationMetersPerSecond2.toFixed(2)} m/s²</dd></div><div><dt>Motion quality</dt><dd>{formatPercent(runtimeState.motionQuality)}</dd></div><div><dt>Lighting estimate</dt><dd>{runtimeState.lightingEstimateAvailable?'AVAILABLE':'--'}</dd></div><div><dt>Primary light magnitude</dt><dd>{runtimeState.primaryLightIntensity?.toFixed(3)??'--'}</dd></div><div><dt>Wake lock</dt><dd>{runtimeState.wakeLockActive?'ACTIVE':runtimeState.wakeLockError??'--'}</dd></div>
      </dl></details>
    </section>
    <StructuralGuideOverlay runtime={runtime} runtimeState={runtimeState} interactionEnabled={running&&!manualMode}/>
    <div className="xr-immersive-overlay" aria-live="polite">
      {running&&manualMode&&<div className="xr-manual-hit-surface" aria-label="Manual corner placement surface" onPointerDown={handleManualPointerDown} onPointerMove={handleManualPointerMove} onPointerUp={handleManualPointerUp} onPointerCancel={handleManualPointerCancel}>
        <div className={`xr-manual-reticle ${manualCursor.phase}`} style={{left:`${manualCursor.u*100}%`,top:`${manualCursor.v*100}%`}} aria-hidden="true"/><div className={`xr-manual-cursor-state ${manualCursor.phase}`} style={{left:`${manualCursor.u*100}%`,top:`${manualCursor.v*100}%`}}>{cursorLabel(manualCursor,runtimeState.manualPointCount)}</div><div className="xr-manual-instruction">Aim at a validated structural edge or corner. If the global worker has no certified target yet, local robust multi-plane geometry may still produce EDGE/CORNER LOCK; ray-verified FREE remains a hard veto.</div>
      </div>}
      {running&&bleActive&&<BleRoughPlacementHud estimates={bleEstimates} pose={runtimeState.pose}/>}
      <div className="xr-overlay-card xr-overlay-card-compact"><div className="xr-overlay-topline"><strong className={runtimeState.motionState==='too-fast'?'bad-text':''}>{phaseLabel(runtimeState)}</strong><span>{runtimeState.wakeLockActive?'SCREEN LOCK ✓':''}</span></div>
        <div className="xr-overlay-scan-grid xr-overlay-scan-grid-compact"><div><span>STABLE</span><strong>{runtimeState.pointCount.toLocaleString()}</strong></div><div><span>READY</span><strong>{readyVisible}</strong></div><div><span>MOTION</span><strong>{formatMotion(runtimeState)}</strong></div><div><span>CPU</span><strong className={computeScheduler.degraded?'bad-text':''}>{computeLabel}</strong></div></div>
        {running&&activeScanIntent==='continue'&&<div className="xr-overlay-hint">CONTINUE MODE · First scan a recognizable area that is already in the saved model from several viewpoints. {continuationSampleReady?'Enough stable points exist for a registration attempt; keep some old area in view before expanding.':`Registration sample ${runtimeState.pointCount.toLocaleString()} / 120 stable points minimum.`} Finish will keep the old SD scan if rigid registration fails.</div>}
        {runtimeState.scanPaused&&<div className="xr-overlay-warning">Scan paused while XR is hidden. It will continue if this XR session becomes visible again.</div>}{runtimeState.motionState==='too-fast'&&<div className="xr-overlay-warning">Move slower — this depth sample is not being fused.</div>}{(runtimeState.motionState==='stationary'||runtimeState.motionState==='redundant')&&<div className="xr-overlay-hint">Move 5–10 cm and change viewpoint. Standing still can polish geometry but cannot increase coverage confidence.</div>}{runtimeState.manualPointError!==null&&<div className="xr-overlay-warning">{runtimeState.manualPointError}</div>}
        <div className="xr-manual-status"><span>Geometry: <strong>{runtimeState.manualPointCount}</strong> pins · <strong>{runtimeState.manualEdgeCount}</strong> edges{certifiedGeometry.hasTargets?` · ready ${certifiedGeometry.cornerCount}C/${certifiedGeometry.edgeCount}E`:''}</span>{manualMode&&<span>Release only on validated EDGE/CORNER LOCK</span>}</div>
        <div className="xr-overlay-actions">{showManualToggle&&<button type="button" className={manualMode?'xr-manual-toggle active':'xr-manual-toggle'} aria-pressed={manualMode} onClick={toggleManualMode}>{manualMode?'Done marking':'Mark corners'}</button>}<button type="button" className="xr-undo-button" disabled={runtimeState.manualPointCount===0} onClick={()=>runtime.undoLastManualPoint()}>Undo pin</button><button type="button" className={aiPriorEnabled?'xr-undo-button active':'xr-undo-button'} aria-pressed={aiPriorEnabled} onClick={toggleAiPrior} title={aiPriorUi.explanation}>AI {aiPriorEnabled?'ON · POST-SCAN':'OFF'}</button></div><button className="xr-finish-button" type="button" onClick={()=>void runtime.stop()}>Finish &amp; save scan</button>
      </div>
    </div>
  </>;
}
