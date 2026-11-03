import { useEffect, useMemo, useRef, useState } from 'react';

import { clientDiagnostics } from '../data/clientDiagnostics';
import { formatErrorText, translate, useLocale, useT } from '../i18n';
import { formatDistance, formatNumber, formatPercent } from '../i18n/format';
import {
  fetchLocalizationRuns,
  isRequestCancellation,
  loadSessionRegistration,
  retryPreemptedRead,
} from '../api/deviceApi';
import type { LocalizationSessionGeometry } from '../api/deviceApi';
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
  manualPlacementErrorLabel,
  manualPreviewLabel,
  previewManualPlacementFromStableSurface,
  type ManualPlacementPreview,
} from '../xr/manualPlacementPreview';
import { optionalMonocularPrior } from '../xr/monocularPriorRuntime';
import { emptyXrPerformanceSnapshot } from '../xr/xrPerformanceProfiler';
import { TagDetectorClient } from '../xr/fiducial/tagDetectorClient';
import { defaultRegistrationTagSizes, decodeRegistrationArtifact, encodeRegistrationArtifact, type RegistrationArtifact } from '../xr/fiducial/registrationArtifact';
import { estimateTagPose } from '../xr/fiducial/tagPose';
import { fuseTagObservations, type TagObservationFusion } from '../xr/fiducial/tagObservationFusion';
import { TagObservationStore } from '../xr/fiducial/tagObservationStore';
import { registerSensorToScan } from '../xr/fiducial/rigidRegistration';
import { confirmedTagIds, newTagScanNotices, registrationArtifactFromResult, registrationOutcome, tagChipStatuses, tagModeErrorLabel, tagScanNoticeText, TAG_FINALIZE_WINDOW_MS, type TagScanNotice } from '../xr/fiducial/tagModeState';
import { registrationApplicability, REGISTRATION_INAPPLICABLE_KEYS, type RegistrationInapplicableReason } from '../xr/registrationApplicability';
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
  manualPointError:null,manualPointErrorKey:null,performance:emptyXrPerformanceSnapshot,lightingEstimateAvailable:false,primaryLightIntensity:null,
  rgbGuidanceAvailable:false,rgbGuidanceWidth:null,rgbGuidanceHeight:null,visualEvidenceFrames:0,monocularPriorStatus:'disabled',depthError:null,error:null,
};

interface XrRuntimePanelProps {
  objectId?: string | null;
  sessionId?: string | null;
  savedScanAvailable?: boolean;
  savedScanBusy?: boolean;
  onScanIntent?: (intent: ScanStartIntent) => boolean;
  onDeleteSavedScan?: () => void;
  onSessionActiveChange?: (active:boolean)=>void;
  onPointCloudReady?: (snapshot:ScanPointCloudSnapshot|null)=>void;
  onCapabilitiesChange?: (capabilities:WebXrCapabilities)=>void;
  /** Restores (Continue, only when applicable) or clears the authoritative registration held by the parent. */
  onRegistrationChange?: (artifact:RegistrationArtifact|null)=>void;
  /** Intent of the scan being started; Fresh never restores the previous registration. */
  scanIntent?: ScanStartIntent;
  /** Identity of the durable base scan a Continue extends; the saved registration must be bound to it. */
  baseScanFrameId?: string|null;
  /**
   * Provisional tag result of this AR session (unbound to any scan). final=true is sent exactly once
   * after Finish, always after onPointCloudReady, and is the signal that tag finalization is done.
   */
  onTagRegistrationResult?: (artifact:RegistrationArtifact|null,final:boolean)=>void;
}
function boundedWait(promise:Promise<unknown>,ms:number):Promise<void>{
  return new Promise<void>((resolve)=>{const timer=setTimeout(resolve,ms);promise.then(()=>{clearTimeout(timer);resolve();},()=>{clearTimeout(timer);resolve();});});
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
function phaseLabel(state:WebXrRuntimeState,locale:'en'|'uk'):string{
  if(state.phase==='starting')return translate(locale,'xr.phase.starting');if(state.phase==='stopping')return translate(locale,'xr.phase.finishing');if(state.phase==='interrupted')return translate(locale,'xr.phase.interrupted');
  if(state.phase==='ended')return translate(locale,state.pointCount>0?'xr.phase.scan_ready':'xr.phase.no_stable_surface');if(state.phase==='error')return translate(locale,'xr.phase.error');if(state.phase!=='running')return translate(locale,'xr.phase.ready');
  if(state.scanPaused)return translate(locale,'xr.phase.paused');if(!state.tracking)return translate(locale,'xr.phase.finding_tracking');if(!state.depthAvailable)return translate(locale,'xr.phase.depth_unavailable');if(state.motionState==='too-fast')return translate(locale,'xr.phase.move_slower');
  if(state.motionState==='stationary'||state.motionState==='redundant')return translate(locale,'xr.phase.new_view');if(!state.coverageGuideActive)return translate(locale,'xr.phase.building_surface');return translate(locale,'xr.phase.scanning');
}
function qualityClass(state:WebXrRuntimeState):string{if(state.phase==='error'||state.phase==='interrupted')return'bad';if(state.phase==='running'&&state.motionState==='too-fast')return'warning';if(state.phase==='running'||state.phase==='ended')return'good';return'';}
function formatMotion(state:WebXrRuntimeState,locale:'en'|'uk'):string{
  if(state.scanPaused)return translate(locale,'xr.phase.paused');switch(state.motionState){case'keyframe':return translate(locale,'xr.motion.new_view');case'polish':return translate(locale,'xr.motion.polishing');case'stationary':return translate(locale,'xr.motion.move_phone');case'redundant':return translate(locale,'xr.motion.new_angle');case'too-fast':return translate(locale,'xr.motion.too_fast');case'initializing':return translate(locale,'xr.motion.initializing');default:return'--';}
}
function pressureLabel(value:string,t:ReturnType<typeof useT>):string{switch(value){case'low':return t('xr.pressure.low');case'medium':return t('xr.pressure.medium');case'high':return t('xr.pressure.high');default:return value;}}
function normalizedPointer(event:React.PointerEvent<HTMLElement>):readonly[number,number]{const rect=event.currentTarget.getBoundingClientRect();const width=Math.max(1,rect.width),height=Math.max(1,rect.height);return[Math.max(0,Math.min(1,(event.clientX-rect.left)/width)),Math.max(0,Math.min(1,(event.clientY-rect.top)/height))];}
function previewPhase(preview:ManualPlacementPreview):ManualCursorPhase{return!preview.resolution.accepted||preview.resolution.position===null?'scan-more':preview.resolution.mode==='corner'?'corner':'edge';}
function previewDistanceMeters(preview:ManualPlacementPreview,state:WebXrRuntimeState):number|null{const target=preview.resolution.position,pose=state.pose;if(target===null||pose===null)return null;return Math.hypot(target[0]-pose.position[0],target[1]+pose.position[2],target[2]-pose.position[1]);}
function cursorLabel(cursor:ManualCursorState,count:number,locale:'en'|'uk',t:ReturnType<typeof useT>):string{if(cursor.phase==='dragging'&&cursor.preview===null)return t('xr.manual.reticle');if(cursor.phase==='confirmed')return t('xr.manual.pin_confirmed',{count:formatNumber(locale,count)});if(cursor.phase==='scan-more'||cursor.preview===null)return t('xr.manual.scan_more');const confidence=formatPercent(locale,cursor.preview.resolution.confidence);const distance=cursor.distanceMeters===null?'':` · ${formatDistance(locale,cursor.distanceMeters)}`;return`${manualPreviewLabel(cursor.preview,locale)} ${confidence}${distance}`;}

export function XrRuntimePanel({
  objectId=null,
  sessionId=null,
  savedScanAvailable=false,
  savedScanBusy=false,
  onScanIntent,
  onDeleteSavedScan,
  onSessionActiveChange,
  onPointCloudReady,
  onCapabilitiesChange,
  onRegistrationChange,
  scanIntent='fresh',
  baseScanFrameId=null,
  onTagRegistrationResult,
}:XrRuntimePanelProps){
  const {locale}=useLocale();const t=useT();
  const runtime=useMemo(()=>new WebXrRuntime(),[]);const[capabilities,setCapabilities]=useState<WebXrCapabilities|null>(null);const[runtimeState,setRuntimeState]=useState<WebXrRuntimeState>(emptyRuntimeState);const[manualMode,setManualMode]=useState(false);const[aiPriorEnabled,setAiPriorEnabled]=useState(readAiPriorPreference);const[computePreparing,setComputePreparing]=useState(false);const[activeScanIntent,setActiveScanIntent]=useState<ScanStartIntent>('fresh');const[,setAiPriorRevision]=useState(0);const[,setCertifiedGeometryRevision]=useState(0);
  const[tagModeEnabled,setTagModeEnabled]=useState(false);const[tagGeometry,setTagGeometry]=useState<LocalizationSessionGeometry|null>(null);const[tagGeometryAt,setTagGeometryAt]=useState<number|null>(null);const[tagGeometryLoading,setTagGeometryLoading]=useState(false);const[tagModeError,setTagModeError]=useState<string|null>(null);const[tagFusion,setTagFusion]=useState<TagObservationFusion>({byTagId:{},rejectedCount:0});const[tagNotices,setTagNotices]=useState<TagScanNotice[]>([]);const tagConfirmedRef=useRef<number[]>([]);const[tagRegistrationLabel,setTagRegistrationLabel]=useState<Parameters<typeof registrationOutcome>[0]>(null);
  const[manualCursor,setManualCursor]=useState<ManualCursorState>({u:.5,v:.5,phase:'scan-more',preview:null,distanceMeters:null});const manualPointer=useRef<ManualPointerState|null>(null);const lastPreviewUpdateMs=useRef(Number.NEGATIVE_INFINITY);const previousManualCount=useRef(0);const previousRuntimePhase=useRef<XrRuntimePhase>('idle');
  const onSessionActiveChangeRef=useRef(onSessionActiveChange),onPointCloudReadyRef=useRef(onPointCloudReady),onCapabilitiesChangeRef=useRef(onCapabilitiesChange),onRegistrationChangeRef=useRef(onRegistrationChange);
  const tagGeometryRef=useRef<{objectId:string;sessionId:string;geometry:LocalizationSessionGeometry;geometryComputedAtMs:number}|null>(null);const tagObservationsRef=useRef(new TagObservationStore());const tagClientRef=useRef<TagDetectorClient|null>(null);const tagFramesUnsubscribeRef=useRef<(()=>void)|null>(null);const tagTaskRef=useRef<Promise<void>|null>(null);const tagModeActiveRef=useRef(false);const tagAcceptPendingRef=useRef(false);const tagStopRef=useRef<(options?:{discard?:boolean})=>Promise<RegistrationArtifact|null>>(async()=>null);const tagGeometryEpoch=useRef(0);
  const scanIntentRef=useRef<ScanStartIntent>(scanIntent);const baseScanFrameIdRef=useRef<string|null>(baseScanFrameId);baseScanFrameIdRef.current=baseScanFrameId;
  const onTagRegistrationResultRef=useRef(onTagRegistrationResult);const sessionTagResultRef=useRef<RegistrationArtifact|null>(null);const tagSessionToken=useRef(0);
  const[staleRegistrationReason,setStaleRegistrationReason]=useState<RegistrationInapplicableReason|null>(null);
  const tagMountedRef=useRef(true);const tagTargetRef=useRef({objectId,sessionId});tagTargetRef.current={objectId,sessionId};const previousTagTargetRef=useRef({objectId,sessionId});
  const bleMountedRef=useRef(true);const bleIntentRef=useRef(0);
  const teardownTagModeForUnmount=():void=>{
    tagModeActiveRef.current=false;tagAcceptPendingRef.current=false;tagFramesUnsubscribeRef.current?.();tagFramesUnsubscribeRef.current=null;runtime.setTagCaptureEnabled(false);
    tagClientRef.current?.dispose();tagClientRef.current=null;tagTaskRef.current=null;tagObservationsRef.current.clear();tagGeometryRef.current=null;
  };
  useEffect(()=>{onSessionActiveChangeRef.current=onSessionActiveChange;},[onSessionActiveChange]);useEffect(()=>{onPointCloudReadyRef.current=onPointCloudReady;},[onPointCloudReady]);useEffect(()=>{onCapabilitiesChangeRef.current=onCapabilitiesChange;},[onCapabilitiesChange]);useEffect(()=>{onRegistrationChangeRef.current=onRegistrationChange;},[onRegistrationChange]);useEffect(()=>{onTagRegistrationResultRef.current=onTagRegistrationResult;},[onTagRegistrationResult]);useEffect(()=>{scanIntentRef.current=scanIntent;},[scanIntent]);
  useEffect(()=>optionalMonocularPrior.subscribe(()=>setAiPriorRevision((revision)=>revision+1)),[]);
  useEffect(()=>subscribeCertifiedGuideRegistry(()=>setCertifiedGeometryRevision((revision)=>revision+1)),[]);
  useEffect(()=>{optionalMonocularPrior.setEnabled(aiPriorEnabled);},[aiPriorEnabled]);
  useEffect(()=>{runtime.attach();tagMountedRef.current=true;bleMountedRef.current=true;const unsubscribe=runtime.subscribe(setRuntimeState);let cancelled=false;void probeWebXrCapabilities().then((result)=>{clientDiagnostics.info('XR_CAPABILITIES',result);void clientDiagnostics.flush();if(!cancelled){setCapabilities(result);onCapabilitiesChangeRef.current?.(result);}});return()=>{cancelled=true;tagMountedRef.current=false;bleMountedRef.current=false;tagGeometryEpoch.current+=1;bleIntentRef.current+=1;teardownTagModeForUnmount();const pointer=manualPointer.current;if(pointer?.timer!=null)window.clearTimeout(pointer.timer);manualPointer.current=null;unsubscribe();void runtime.detach();};},[runtime]);
  useEffect(()=>{tagGeometryEpoch.current+=1;tagGeometryRef.current=null;setTagGeometry(null);setTagGeometryAt(null);setTagGeometryLoading(false);setTagModeError(null);setTagRegistrationLabel(null);return()=>{tagGeometryEpoch.current+=1;};},[objectId,sessionId]);
  useEffect(()=>{const active=runtimeState.phase==='starting'||runtimeState.phase==='running'||runtimeState.phase==='stopping';onSessionActiveChangeRef.current?.(active);if(!active){setManualMode(false);manualPointer.current=null;}},[runtimeState.phase]);
  useEffect(()=>{
    const previous=previousRuntimePhase.current;const previousTarget=previousTagTargetRef.current;const targetChanged=previousTarget.objectId!==objectId||previousTarget.sessionId!==sessionId;
    previousRuntimePhase.current=runtimeState.phase;previousTagTargetRef.current={objectId,sessionId};
    const bootstrapping=runtimeState.phase==='starting'&&previous!=='starting';
    const retargeting=targetChanged&&(runtimeState.phase==='starting'||runtimeState.phase==='running');
    if(bootstrapping||retargeting){
      if(bootstrapping)onPointCloudReadyRef.current?.(null);
      const epoch=++tagGeometryEpoch.current;const runtimeGeneration=runtime.getLifecycleGeneration();tagGeometryRef.current=null;setTagGeometry(null);setTagGeometryAt(null);setTagGeometryLoading(true);setTagModeError(null);setTagRegistrationLabel(null);setStaleRegistrationReason(null);if(bootstrapping)sessionTagResultRef.current=null;
      const ownsRequest=():boolean=>tagMountedRef.current&&epoch===tagGeometryEpoch.current&&tagTargetRef.current.objectId===objectId&&tagTargetRef.current.sessionId===sessionId&&runtime.getLifecycleGeneration()===runtimeGeneration&&['starting','running'].includes(runtime.getState().phase);
      if(objectId===null||sessionId===null){setTagGeometryLoading(false);setTagModeError('Open a recording before using tag registration.');return;}
      void (async()=>{
        try{
          const listing=await retryPreemptedRead(()=>fetchLocalizationRuns(objectId,sessionId));if(!ownsRequest())return;
          const geometry=listing.geometry;const geometryComputedAtMs=Math.max(0,...geometry.pods.filter((pod)=>pod.computed).map((pod)=>pod.computed_at_ms??0));
          tagGeometryRef.current={objectId,sessionId,geometry,geometryComputedAtMs};setTagGeometry(geometry);setTagGeometryAt(geometryComputedAtMs);setTagGeometryLoading(false);
          /* MAX-405: Fresh replaces the scan frame, so the previous registration is never restored for it. */
          if(scanIntentRef.current!=='continue')return;
          try{
            const registrationJson=await loadSessionRegistration(objectId,sessionId);if(!ownsRequest())return;
            const registration=registrationJson===null?null:decodeRegistrationArtifact(registrationJson);
            const applicability=registrationApplicability(registration,{geometryComputedAtMs:geometryComputedAtMs>0?geometryComputedAtMs:null,scanFrameId:baseScanFrameIdRef.current});
            if(registration===null||applicability.ok){onRegistrationChangeRef.current?.(registration);if(registration!==null)setTagRegistrationLabel(registration);}
            else{onRegistrationChangeRef.current?.(null);setStaleRegistrationReason(applicability.reason);}
          }catch(reason){if(!ownsRequest())return;if(isRequestCancellation(reason))return;setTagModeError(`Saved anchor status unavailable: ${reason instanceof Error?reason.message:String(reason)}`);}
        }catch(reason){if(!ownsRequest())return;setTagGeometryLoading(false);if(isRequestCancellation(reason))return;setTagModeError(`Frozen session geometry unavailable: ${reason instanceof Error?reason.message:String(reason)}`);}
      })();
    }else if((runtimeState.phase==='ended'||runtimeState.phase==='interrupted')&&previous!==runtimeState.phase){
      tagGeometryEpoch.current+=1;
      const finished=runtimeState.phase==='ended'&&runtimeState.pointCount>0;
      /* MAX-403: the Finished scan is primary evidence and is handed over at once; tag finalization is a separate bounded stage. */
      if(finished&&tagMountedRef.current)onPointCloudReadyRef.current?.(runtime.getPointCloudSnapshot());
      void (async()=>{
        let artifact:RegistrationArtifact|null=null;
        try{artifact=await tagStopRef.current({discard:!finished});}catch{artifact=null;}
        if(!tagMountedRef.current||!finished)return;
        onTagRegistrationResultRef.current?.(artifact??sessionTagResultRef.current,true);
      })();
    }else if(runtimeState.phase==='error'&&previous!==runtimeState.phase){tagGeometryEpoch.current+=1;void tagStopRef.current({discard:true});}
  },[runtime,runtimeState.phase,runtimeState.pointCount,objectId,sessionId]);
  useEffect(()=>{if(runtimeState.manualPointCount>previousManualCount.current)setManualCursor((current)=>({...current,phase:'confirmed'}));previousManualCount.current=runtimeState.manualPointCount;},[runtimeState.manualPointCount]);
  useEffect(()=>{if(runtimeState.manualPointError!==null||runtimeState.manualPointErrorKey!==null)setManualCursor((current)=>({...current,phase:'scan-more'}));},[runtimeState.manualPointError,runtimeState.manualPointErrorKey]);
  /* MAX-146: optional BLE RSSI node hints. Samples are only taken with a live XR pose and reset per session (new reference frame). */
  const bleNavigator=typeof navigator==='undefined'?undefined:(navigator as unknown as Parameters<typeof bleRssiSupport>[0]);
  const bleSupport=useMemo(()=>bleRssiSupport(bleNavigator,typeof window!=='undefined'&&window.isSecureContext),[bleNavigator]);
  const blePlacement=useMemo(()=>new BleRoughPlacement(),[]);const bleSource=useMemo(()=>new WebBluetoothRssiSource(bleNavigator),[bleNavigator]);
  const[bleActive,setBleActive]=useState(false);const[bleTick,setBleTick]=useState(0);const posePointer=useRef<WebXrPoseSnapshot|null>(null);
  useEffect(()=>{posePointer.current=runtimeState.phase==='running'&&runtimeState.tracking?runtimeState.pose:null;},[runtimeState.phase,runtimeState.tracking,runtimeState.pose]);
  useEffect(()=>()=>bleSource.dispose(),[bleSource]);
  useEffect(()=>{if(runtimeState.phase==='starting')blePlacement.reset();else if(runtimeState.phase==='ended'||runtimeState.phase==='interrupted'||runtimeState.phase==='error'){if(bleSource.active){clientDiagnostics.info('XR_BLE_RSSI_DIAGNOSTICS',blePlacement.diagnostics(Date.now()));void clientDiagnostics.flush();}bleIntentRef.current+=1;bleSource.stop();if(bleMountedRef.current)setBleActive(false);}},[runtimeState.phase,blePlacement,bleSource]);
  useEffect(()=>{if(!bleActive||runtimeState.phase!=='running')return;const id=window.setInterval(()=>setBleTick((tick)=>tick+1),1000);return()=>window.clearInterval(id);},[bleActive,runtimeState.phase]);
  const bleEstimates=useMemo(()=>(bleActive&&bleTick>=0?blePlacement.estimates(Date.now()):[]),[blePlacement,bleActive,bleTick]);
  const toggleBle=async():Promise<void>=>{
    if(runtimeState.phase!=='running')return;
    if(bleSource.active||bleSource.starting){bleIntentRef.current+=1;bleSource.stop();setBleActive(false);clientDiagnostics.info('XR_BLE_RSSI',{enabled:false});return;}
    const intent=++bleIntentRef.current;
    const started=await bleSource.start((advert)=>{const pose=posePointer.current;if(pose===null)return;blePlacement.addSample({...advert,phonePosition:pose.position,timestampMs:Date.now()});});
    if(!bleMountedRef.current||intent!==bleIntentRef.current)return;setBleActive(started);clientDiagnostics.info('XR_BLE_RSSI',{enabled:started,support:bleSupport});void clientDiagnostics.flush();
  };

  const stopTagMode=async(options:{discard?:boolean}={}):Promise<RegistrationArtifact|null>=>{
    if(!tagModeActiveRef.current)return null;
    tagAcceptPendingRef.current=options.discard!==true;tagModeActiveRef.current=false;setTagModeEnabled(false);runtime.setTagCaptureEnabled(false);tagFramesUnsubscribeRef.current?.();tagFramesUnsubscribeRef.current=null;
    const pending=tagTaskRef.current;
    /* A stalled detector gets a bounded window; its worker is then disposed and a late result is ignored (capture is already inactive). */
    if(pending!==null&&options.discard!==true)await boundedWait(pending,TAG_FINALIZE_WINDOW_MS);
    tagAcceptPendingRef.current=false;tagTaskRef.current=null;
    tagClientRef.current?.dispose();tagClientRef.current=null;
    if(!tagMountedRef.current||options.discard===true)return null;
    const bound=tagGeometryRef.current;
    if(bound===null||bound.objectId!==objectId||bound.sessionId!==sessionId){setTagModeError('Frozen session geometry is unavailable; tag observations were not registered.');return null;}
    const fusion=fuseTagObservations(tagObservationsRef.current.getObservations(),{minObservations:5,minIndependentViews:3});
    tagObservationsRef.current.recordOutliers(fusion.rejectedCount);setTagFusion(fusion);
    clientDiagnostics.info('XR_TAG_OBSERVATION_STORE_FINALIZED',{retained:tagObservationsRef.current.retainedCount,rejected:tagObservationsRef.current.rejectedCount,outliers:tagObservationsRef.current.outlierCount,evicted:tagObservationsRef.current.evictedCount});void clientDiagnostics.flush();
    const result=registerSensorToScan({tags:fusion.byTagId,geometry:bound.geometry,geometryComputedAtMs:bound.geometryComputedAtMs});
    const artifact=registrationArtifactFromResult(result,fusion,defaultRegistrationTagSizes(),bound.geometryComputedAtMs,Date.now());
    /* Persistence is a separate stage owned by the parent after the scan commit (it needs the committed scan id). */
    try{encodeRegistrationArtifact(artifact);}catch{setTagModeError('Registration artifact was not accepted.');return null;}
    setTagRegistrationLabel(result);setTagModeError(null);
    return artifact;
  };

  tagStopRef.current=stopTagMode;
  const startTagMode=():void=>{
    const bound=tagGeometryRef.current;
    if(bound===null||bound.objectId!==objectId||bound.sessionId!==sessionId||!bound.geometry.available||bound.geometryComputedAtMs<=0){setTagModeError('Frozen localization geometry is unavailable for this recording.');return;}
    const captureError=runtime.setTagCaptureEnabled(true);if(captureError!==null){setTagModeError(captureError);return;}
    const client=new TagDetectorClient();
    if(!client.workerBacked){client.dispose();runtime.setTagCaptureEnabled(false);setTagModeError('AprilTag detector worker could not start.');return;}
    tagObservationsRef.current.clear();tagAcceptPendingRef.current=false;setTagFusion({byTagId:{},rejectedCount:0});setTagNotices([]);tagConfirmedRef.current=[];setTagRegistrationLabel(null);setTagModeError(null);
    const token=++tagSessionToken.current;
    tagClientRef.current=client;tagModeActiveRef.current=true;setTagModeEnabled(true);
    tagFramesUnsubscribeRef.current=runtime.subscribeTagFrames((frame)=>{
      if(!tagModeActiveRef.current||client.busy)return;
      // The capture marks its fresh RGBA buffer as owned; detect transfers that buffer to the worker.
      const task=client.detect(frame).then((detected)=>{
        if(detected===null||(!tagModeActiveRef.current&&!tagAcceptPendingRef.current)||tagSessionToken.current!==token||!tagMountedRef.current)return;
        for(const detection of detected.detections){
          const size=defaultRegistrationTagSizes()[String(detection.tagId) as '0'|'1'|'2'|'3'|'4'];
          const pose=estimateTagPose({corners:detection.corners,projectionMatrix:detected.projectionMatrix,worldFromView:detected.worldFromView,tagSizeM:size,capturedAtMs:detected.capturedAtMs,cameraPosition:detected.cameraPosition,cameraForward:detected.cameraForward});
          if(pose.ok)tagObservationsRef.current.insert({tagId:detection.tagId,positionScan:pose.positionScan,capturedAtMs:pose.capturedAtMs,cameraPosition:pose.cameraPosition,cameraForward:pose.cameraForward});
          else tagObservationsRef.current.recordRejected();
        }
        if(detected.detections.length===0)return;
        setTagFusion(fuseTagObservations(tagObservationsRef.current.getObservations(),{minObservations:5,minIndependentViews:3}));
      }).catch((reason)=>{if(tagModeActiveRef.current)setTagModeError(`AprilTag detection failed: ${reason instanceof Error?reason.message:String(reason)}`);});
      tagTaskRef.current=task;void task.finally(()=>{if(tagTaskRef.current===task)tagTaskRef.current=null;});
    });
  };

  const toggleTagMode=():void=>{
    if(!tagModeActiveRef.current){startTagMode();return;}
    void stopTagMode().then((artifact)=>{if(artifact===null||!tagMountedRef.current)return;sessionTagResultRef.current=artifact;onTagRegistrationResultRef.current?.(artifact,false);});
  };
  const tagChips=tagChipStatuses(tagFusion,locale);
  /* A device whose tag has just been confirmed by the live scan is announced, with the pairs whose distance is recalculated from the scan. */
  useEffect(()=>{const added=newTagScanNotices(tagConfirmedRef.current,tagFusion);tagConfirmedRef.current=confirmedTagIds(tagFusion);if(added.length>0)setTagNotices((previous)=>[...previous,...added].slice(-3));},[tagFusion]);
  const tagUnavailableReason=objectId===null||sessionId===null?t('xr.tag.open_recording'):tagGeometryLoading?t('xr.tag.loading_geometry'):tagGeometry===null||!tagGeometry.available||tagGeometryAt===null||tagGeometryAt<=0?t('xr.tag.geometry_unavailable'):capabilities?.rawXrRgbApi===false?t('xr.tag.camera_unavailable'):capabilities!==null&&capabilities.tagReadbackMode!=='nonblocking'?t('xr.tag.readback_unavailable'):null;

  const running=runtimeState.phase==='running',busy=computePreparing||runtimeState.phase==='starting'||runtimeState.phase==='stopping';const canStart=canAttemptImmersiveAr(capabilities)&&!running&&!busy;const readyVisible=`${formatNumber(locale,runtimeState.coverageReadyCells)} / ${formatNumber(locale,runtimeState.coverageVisibleCells)}`;
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
  },locale);
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
    setActiveScanIntent(intent);scanIntentRef.current=intent;sessionTagResultRef.current=null;
    setComputePreparing(true);
    try{
      await launchImmersiveArFromUserGesture({beginScan:()=>liveComputeScheduler.beginScan(),startRuntime:()=>runtime.start(),prepareCompute:()=>liveComputeScheduler.prepare(),onPrepared:(prepared)=>{clientDiagnostics.info('XR_COMPUTE_BUDGET_PREPARED',{maximumWorkers:prepared.maximumBudget,effectiveWorkers:prepared.effectiveBudget,pressure:prepared.pressure,degraded:prepared.degraded,scanIntent:intent});void clientDiagnostics.flush();}});
    }finally{setComputePreparing(false);}
  };

  const continuationSampleReady=runtimeState.pointCount>=120;

  return <>
    <section className="xr-runtime-card xr-runtime-card-compact" aria-label={t('xr.title')}>
      <div className="xr-runtime-heading"><div><div className="panel-title">{t('xr.heading')}</div><strong>{t('xr.subtitle')}</strong></div><span className={`xr-phase ${qualityClass(runtimeState)}`}>{phaseLabel(runtimeState,locale)}</span></div>
      <div className="xr-compact-status"><span className={capabilities?.immersiveAr?'good-text':'bad-text'}>WebXR {capabilities?.immersiveAr?'✓':capabilities===null?t('xr.value.checking'):'—'}</span><span className={runtimeState.depthAvailable?'good-text':''}>{t('xr.label.depth')} {runtimeState.depthAvailable?'✓':runtimeState.phase==='idle'?t('xr.value.on_start'):'—'}</span><span className={runtimeState.rgbGuidanceAvailable?'good-text':''}>{t('xr.label.rgb')} {runtimeState.rgbGuidanceAvailable?'✓':capabilities?.rawXrRgbApi?t('xr.value.on_start'):'—'}</span><span className={aiPriorUi.good?'good-text':''}>{t('xr.label.ai_prior')} {aiPriorUi.shortStatus}</span><span className={runtimeState.wakeLockActive?'good-text':''}>{t('xr.label.screen')} {runtimeState.wakeLockActive?t('xr.value.awake'):capabilities?.wakeLockApi?t('xr.value.wake_ready'):t('xr.value.wake_lock_off')}</span></div>
      <div className="xr-runtime-controls xr-runtime-controls-compact">
        {savedScanAvailable ? <>
          <button type="button" className="xr-start-button" disabled={!canStart||savedScanBusy} onClick={()=>void startPreparedScan('continue')}>{computePreparing?t('xr.starting_ar'):t('xr.continue_scan')}</button>
          <button type="button" className="xr-undo-button" disabled={!canStart||savedScanBusy} onClick={()=>void startPreparedScan('fresh')}>{t('xr.scan_again')}</button>
          {onDeleteSavedScan!==undefined&&<button type="button" className="xr-undo-button" disabled={busy||savedScanBusy} onClick={onDeleteSavedScan}>{t('xr.delete_saved_scan')}</button>}
          <div className="xr-finish-explainer">{t('xr.continue_description')}</div>
        </> : <>
          <button type="button" className="xr-start-button" disabled={!canStart||savedScanBusy} onClick={()=>void startPreparedScan('fresh')}>{computePreparing?t('xr.starting_ar'):runtimeState.phase==='interrupted'?t('xr.discard_partial_start'):t('xr.start_scan')}</button>
        </>}
        <button type="button" className={aiPriorEnabled?'xr-undo-button active':'xr-undo-button'} aria-pressed={aiPriorEnabled} onClick={toggleAiPrior} title={aiPriorUi.explanation}>{aiPriorEnabled?t('xr.ai_check_on'):t('xr.ai_check_off')}</button>
        {bleSupport==='supported'?<button type="button" className={bleActive?'xr-undo-button active':'xr-undo-button'} aria-pressed={bleActive} disabled={runtimeState.phase!=='running'} onClick={()=>void toggleBle()} title={t('xr.ble_hints_title')}>{bleActive?t('xr.ble_hints_on'):t('xr.ble_hints_off')}</button>:<div className="xr-finish-explainer">{describeBleRssiSupport(bleSupport,locale)}</div>}
        <div className="xr-finish-explainer">{t('xr.ai_optional_description')}</div>
      </div>
      {aiPriorEnabled&&optionalMonocularPrior.status==='unavailable'&&<div className="xr-runtime-warning" role="status">{t('xr.ai_unavailable_warning')}</div>}
      {aiPriorEnabled&&optionalMonocularPrior.status==='error'&&<div className="xr-runtime-warning" role="status">{t('xr.ai_failed_warning',{error:optionalMonocularPrior.error??t('calc.job_unknown_error')})}</div>}
      {runtimeState.phase==='interrupted'&&<div className="xr-runtime-warning" role="status">{t('xr.session_interrupted')}</div>}
      {runtimeState.depthError!==null&&<div className="xr-runtime-warning" role="status">{formatErrorText(locale,runtimeState.depthError)}</div>}{runtimeState.error!==null&&<div className="xr-runtime-error" role="alert">{formatErrorText(locale,runtimeState.error)}</div>}
      <details className="xr-technical-details"><summary>{t('scan.details.technical')}</summary><dl className="xr-runtime-values">
        <div><dt>{t('xr.detail.depth_type')}</dt><dd>{runtimeState.depthType??'--'}</dd></div><div><dt>{t('xr.detail.depth_usage')}</dt><dd>{runtimeState.depthUsage??'--'}</dd></div><div><dt>{t('xr.detail.depth_format')}</dt><dd>{runtimeState.depthDataFormat??'--'}</dd></div><div><dt>{t('xr.detail.depth_size')}</dt><dd>{runtimeState.depthWidth&&runtimeState.depthHeight?`${formatNumber(locale,runtimeState.depthWidth)} × ${formatNumber(locale,runtimeState.depthHeight)}`:'--'}</dd></div>
        <div><dt>{t('xr.detail.raw_rgb')}</dt><dd>{capabilities?.rawXrRgbApi?t('scan.compute.available'):'--'}</dd></div><div><dt>{t('xr.detail.rgb_guidance')}</dt><dd>{runtimeState.rgbGuidanceAvailable?`${formatNumber(locale,runtimeState.rgbGuidanceWidth??0)} × ${formatNumber(locale,runtimeState.rgbGuidanceHeight??0)}`:'--'}</dd></div><div><dt>{t('xr.detail.video_inputs')}</dt><dd>{capabilities?.videoInputsEnumerated?formatNumber(locale,capabilities.videoInputCount??0):capabilities?.videoInputEnumeration?t('xr.value.not_exposed'):'--'}</dd></div><div><dt>{t('xr.detail.hardware_depth_sensor')}</dt><dd>{capabilities?.hardwareDepthSensor?.toUpperCase()??t('xr.value.unknown')} · {t('xr.hardware_depth_not_inferred')}</dd></div><div><dt>{t('xr.detail.visual_evidence_frames')}</dt><dd>{formatNumber(locale,runtimeState.visualEvidenceFrames)}</dd></div><div><dt>{t('xr.detail.ai_depth_prior')}</dt><dd>{aiPriorUi.shortStatus}</dd></div><div><dt>{t('xr.detail.ai_calibration')}</dt><dd>{aiPriorUi.calibrationLabel}</dd></div>
        <div><dt>{t('xr.detail.stable_points')}</dt><dd>{formatNumber(locale,runtimeState.pointCount)}</dd></div><div><dt>{t('xr.detail.retained_hypotheses')}</dt><dd>{formatNumber(locale,runtimeState.retainedPointCount)}</dd></div><div><dt>{t('scan.details.keyframes')}</dt><dd>{formatNumber(locale,runtimeState.keyframeCount)}</dd></div><div><dt>{t('scan.details.polish_frames')}</dt><dd>{formatNumber(locale,runtimeState.polishFrameCount)}</dd></div><div><dt>{t('xr.detail.motion_rejects')}</dt><dd>{formatNumber(locale,runtimeState.motionRejectedFrames)}</dd></div>
        <div><dt>{t('xr.detail.ready_visible_cells')}</dt><dd>{readyVisible}</dd></div><div><dt>{t('xr.detail.coverage_confidence')}</dt><dd>{formatPercent(locale,runtimeState.coverageAverageConfidence)}</dd></div><div><dt>{t('xr.detail.weak_points')}</dt><dd>{formatNumber(locale,runtimeState.coverageWeakPoints)}</dd></div><div><dt>{t('xr.detail.improving_points')}</dt><dd>{formatNumber(locale,runtimeState.coverageImprovingPoints)}</dd></div><div><dt>{t('xr.detail.stable_guide_points')}</dt><dd>{formatNumber(locale,runtimeState.coverageStablePoints)}</dd></div>
        <div><dt>{t('xr.detail.worker_api')}</dt><dd>{capabilities?.workerApi?t('scan.compute.available'):'--'}</dd></div><div><dt>{t('xr.detail.shared_memory')}</dt><dd>{capabilities?.sharedMemory?t('scan.compute.available'):'--'}</dd></div><div><dt>{t('xr.detail.wasm_threads')}</dt><dd>{capabilities?.wasmThreads?t('scan.compute.available'):'--'}</dd></div><div><dt>WebGPU</dt><dd>{capabilities?.webGpu?t('scan.compute.available'):'--'}</dd></div><div><dt>{t('xr.detail.live_cpu_workers')}</dt><dd>{computeLabel} · {pressureLabel(computeScheduler.pressure,t)}{computeScheduler.degraded?` ${t('xr.value.throttled')}`:''}</dd></div><div><dt>{t('xr.detail.active_heavy_jobs')}</dt><dd>{formatNumber(locale,computeScheduler.active)} · S{formatNumber(locale,computeScheduler.structural)} / V{formatNumber(locale,computeScheduler.visual)}</dd></div><div><dt>{t('xr.detail.manual_corners')}</dt><dd>{formatNumber(locale,runtimeState.manualPointCount)}</dd></div><div><dt>{t('xr.detail.confirmed_edges')}</dt><dd>{formatNumber(locale,runtimeState.manualEdgeCount)}</dd></div><div><dt>{t('xr.detail.frame_p95')}</dt><dd>{formatNumber(locale,runtimeState.performance.frameDeltaP95Ms,{minimumFractionDigits:1,maximumFractionDigits:1})} ms</dd></div><div><dt>{t('xr.detail.fusion_p50_p95')}</dt><dd>{formatNumber(locale,runtimeState.performance.fusionP50Ms,{minimumFractionDigits:1,maximumFractionDigits:1})} / {formatNumber(locale,runtimeState.performance.fusionP95Ms,{minimumFractionDigits:1,maximumFractionDigits:1})} ms</dd></div><div><dt>{t('xr.detail.coverage_p95')}</dt><dd>{formatNumber(locale,runtimeState.performance.coverageP95Ms,{minimumFractionDigits:1,maximumFractionDigits:1})} ms</dd></div><div><dt>{t('xr.detail.long_frames')}</dt><dd>{formatNumber(locale,runtimeState.performance.longFrames33Ms)} / {formatNumber(locale,runtimeState.performance.longFrames50Ms)} ms</dd></div><div><dt>{t('xr.detail.live_render_pressure')}</dt><dd>{pressureLabel(runtimeState.performance.pressure,t)} · {formatNumber(locale,runtimeState.performance.adaptiveCoverageIntervalMs)} ms</dd></div>
        <div><dt>{t('xr.detail.linear_speed')}</dt><dd>{formatNumber(locale,runtimeState.linearSpeedMetersPerSecond,{minimumFractionDigits:2,maximumFractionDigits:2})} m/s</dd></div><div><dt>{t('xr.detail.angular_speed')}</dt><dd>{formatNumber(locale,runtimeState.angularSpeedRadiansPerSecond,{minimumFractionDigits:2,maximumFractionDigits:2})} rad/s</dd></div><div><dt>{t('xr.detail.acceleration')}</dt><dd>{formatNumber(locale,runtimeState.linearAccelerationMetersPerSecond2,{minimumFractionDigits:2,maximumFractionDigits:2})} m/s²</dd></div><div><dt>{t('xr.detail.motion_quality')}</dt><dd>{formatPercent(locale,runtimeState.motionQuality)}</dd></div><div><dt>{t('xr.detail.lighting_estimate')}</dt><dd>{runtimeState.lightingEstimateAvailable?t('scan.compute.available'):'--'}</dd></div><div><dt>{t('xr.detail.primary_light')}</dt><dd>{runtimeState.primaryLightIntensity===null?'--':formatNumber(locale,runtimeState.primaryLightIntensity,{minimumFractionDigits:3,maximumFractionDigits:3})}</dd></div><div><dt>{t('xr.detail.wake_lock')}</dt><dd>{runtimeState.wakeLockActive?t('xr.value.active'):runtimeState.wakeLockError??'--'}</dd></div>
        <div><dt>{t('xr.detail.tag_readback')}</dt><dd>{capabilities?.tagReadbackMode==='nonblocking'?t('xr.value.tag_readback_nonblocking'):capabilities?.tagReadbackMode==='synchronous'?t('xr.value.tag_readback_synchronous'):capabilities?.tagReadbackMode==='unavailable'?t('xr.value.tag_readback_unavailable'):'--'}</dd></div>
      </dl></details>
    </section>
    <StructuralGuideOverlay runtime={runtime} runtimeState={runtimeState} interactionEnabled={running&&!manualMode}/>
    <div className="xr-immersive-overlay" aria-live="polite">
      {running&&manualMode&&<div className="xr-manual-hit-surface" aria-label={t('xr.manual.manual_surface')} onPointerDown={handleManualPointerDown} onPointerMove={handleManualPointerMove} onPointerUp={handleManualPointerUp} onPointerCancel={handleManualPointerCancel}>
        <div className={`xr-manual-reticle ${manualCursor.phase}`} style={{left:`${manualCursor.u*100}%`,top:`${manualCursor.v*100}%`}} aria-hidden="true"/><div className={`xr-manual-cursor-state ${manualCursor.phase}`} style={{left:`${manualCursor.u*100}%`,top:`${manualCursor.v*100}%`}}>{cursorLabel(manualCursor,runtimeState.manualPointCount,locale,t)}</div><div className="xr-manual-instruction">{t('xr.manual.instruction')}</div>
      </div>}
      {running&&bleActive&&<BleRoughPlacementHud estimates={bleEstimates} pose={runtimeState.pose}/>}
      <div className="xr-overlay-card xr-overlay-card-compact"><div className="xr-overlay-topline"><strong className={runtimeState.motionState==='too-fast'?'bad-text':''}>{phaseLabel(runtimeState,locale)}</strong><span>{runtimeState.wakeLockActive?t('xr.overlay.screen_lock'):''}</span></div>
        <div className="xr-overlay-scan-grid xr-overlay-scan-grid-compact"><div><span>{t('xr.overlay.stable')}</span><strong>{formatNumber(locale,runtimeState.pointCount)}</strong></div><div><span>{t('xr.overlay.ready')}</span><strong>{readyVisible}</strong></div><div><span>{t('xr.overlay.motion')}</span><strong>{formatMotion(runtimeState,locale)}</strong></div><div><span>{t('xr.overlay.cpu')}</span><strong className={computeScheduler.degraded?'bad-text':''}>{computeLabel}</strong></div></div>
        {running&&activeScanIntent==='continue'&&<div className="xr-overlay-hint">{t('xr.overlay.continue_hint',{sample:continuationSampleReady?t('xr.overlay.continue_enough'):t('xr.overlay.registration_sample',{count:formatNumber(locale,runtimeState.pointCount)})})}</div>}
        {runtimeState.scanPaused&&<div className="xr-overlay-warning">{t('xr.manual.paused')}</div>}{runtimeState.motionState==='too-fast'&&<div className="xr-overlay-warning">{t('xr.manual.move_slower')}</div>}{(runtimeState.motionState==='stationary'||runtimeState.motionState==='redundant')&&<div className="xr-overlay-hint">{t('xr.manual.move_viewpoint')}</div>}{runtimeState.manualPointErrorKey!==null&&<div className="xr-overlay-warning" role="status">{t(runtimeState.manualPointErrorKey)}</div>}{runtimeState.manualPointErrorKey===null&&runtimeState.manualPointError!==null&&<div className="xr-overlay-warning">{manualPlacementErrorLabel(runtimeState.manualPointError,locale)}</div>}
        <div className="xr-manual-status"><span>{t('xr.manual.geometry_count',{points:formatNumber(locale,runtimeState.manualPointCount),edges:formatNumber(locale,runtimeState.manualEdgeCount),ready:certifiedGeometry.hasTargets?t('xr.manual.geometry_ready',{corners:formatNumber(locale,certifiedGeometry.cornerCount),edges:formatNumber(locale,certifiedGeometry.edgeCount)}):''})}</span>{manualMode&&<span>{t('xr.manual.release_rule')}</span>}</div>
        <div className="xr-overlay-actions">{showManualToggle&&<button type="button" className={manualMode?'xr-manual-toggle active':'xr-manual-toggle'} aria-pressed={manualMode} onClick={toggleManualMode}>{manualMode?t('xr.manual.done_marking'):t('xr.manual.mark_corners')}</button>}<button type="button" className={tagModeEnabled?'xr-manual-toggle active':'xr-manual-toggle'} aria-pressed={tagModeEnabled} disabled={!running||(!tagModeEnabled&&tagUnavailableReason!==null)} title={tagModeEnabled?t('xr.tag.tags_on'):t('xr.tag.capture_title')} onClick={toggleTagMode}>{tagModeEnabled?t('xr.tag.tags_on'):t('xr.tag.tags')}</button><button type="button" className="xr-undo-button" disabled={runtimeState.manualPointCount===0} onClick={()=>runtime.undoLastManualPoint()}>{t('xr.manual.undo_pin')}</button><button type="button" className={aiPriorEnabled?'xr-undo-button active':'xr-undo-button'} aria-pressed={aiPriorEnabled} onClick={toggleAiPrior} title={aiPriorUi.explanation}>{aiPriorEnabled?t('xr.ai_check_on'):t('xr.ai_check_off')}</button></div>
        {running&&!tagModeEnabled&&tagUnavailableReason!==null&&<div className="xr-overlay-hint">{t('xr.tag.unavailable',{reason:tagUnavailableReason})}</div>}
        {tagModeEnabled&&tagNotices.length>0&&<div className="xr-tag-notices" role="status" aria-live="polite">{tagNotices.map((notice)=><div key={notice.tagId} className="xr-overlay-hint xr-tag-notice" data-node={notice.node}>{tagScanNoticeText(notice,locale)}</div>)}</div>}
        {tagModeEnabled&&<div className="xr-manual-status" aria-label={t('xr.tag.observations')}>{tagChips.map((chip)=><span key={chip.tagId} className={`xr-manual-toggle ${chip.status}`}>{chip.text}</span>)}</div>}
        {tagModeError!==null&&<div className="xr-overlay-warning" role="status">{tagModeErrorLabel(tagModeError,locale)}</div>}
        {staleRegistrationReason!==null&&<div className="xr-overlay-warning" role="status">{t('xr.tag.registration.stale',{reason:t(REGISTRATION_INAPPLICABLE_KEYS[staleRegistrationReason])})}</div>}
        {tagRegistrationLabel!==null&&<div className="xr-overlay-hint" role="status">{registrationOutcome(tagRegistrationLabel,locale)}</div>}
        <button className="xr-finish-button" type="button" disabled={!running} onClick={()=>void runtime.stop()}>{t('xr.finish_save')}</button>
      </div>
    </div>
  </>;
}
