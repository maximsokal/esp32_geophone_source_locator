import { useEffect, useMemo, useRef, useState } from 'react';

import { clientDiagnostics } from '../data/clientDiagnostics';
import { clearScanLaunchIntent, consumeScanLaunchIntent, SCAN_LAUNCH_STAGING_SESSION } from '../xr/scanLaunchIntent';
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
import { LiveTagTracker, tagSensorAnchorsFromFusion, type TagSensorAnchor } from '../xr/fiducial/liveTagTracker';
import { tagNodeForId } from '../xr/fiducial/tagBindings.gen';
import { LiveTagOverlay } from './LiveTagOverlay';
import { defaultRegistrationTagSizes, decodeRegistrationArtifact, encodeRegistrationArtifact, type RegistrationArtifact } from '../xr/fiducial/registrationArtifact';
import { estimateTagPose } from '../xr/fiducial/tagPose';
import { fuseTagObservations, type TagObservation, type TagObservationFusion } from '../xr/fiducial/tagObservationFusion';
import { LIVE_MESH_MODES, readLiveMeshMode, writeLiveMeshMode } from '../xr/liveMeshSetting';
import type { LiveMeshMode } from '../xr/webxrRuntime';
import type { RelockSummary } from '../xr/continuationOutcome';
import { evaluateRelock, INITIAL_RELOCK_STATE, recentTagObservations, relockBaseTags, relockLostByJump, relockStillHolds, RELOCK_MIN_OBSERVATIONS, RELOCK_MIN_TAGS, RELOCK_MIN_VIEWS, type RelockBaseTag, type RelockState, type RelockTransform } from '../xr/fiducial/continuationRelocalization';
import type { TagPositionsArtifact } from '../xr/fiducial/tagPositionsArtifact';
import { buildReferenceMeshOverlay, buildReferenceOverlay } from '../xr/fiducial/referenceOverlay';
import { TagObservationStore } from '../xr/fiducial/tagObservationStore';
import { registerSensorToScan } from '../xr/fiducial/rigidRegistration';
import { geophonePositionsFromTags, geophonePositionsRegistrationBlock } from '../xr/fiducial/geophonePositions';
import { confirmedTagIds, frameJumpTagIds, mergeTagNotices, newFrameJumpNotices, newTagScanNotices, registrationArtifactFromResult, registrationOutcome, tagModeErrorLabel, tagScanNoticeText, TAG_FINALIZE_WINDOW_MS, type TagScanNotice } from '../xr/fiducial/tagModeState';
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


export type ScanStartIntent = 'fresh' | 'continue';

const emptyRuntimeState: WebXrRuntimeState = {
  fusionCatchingUp: false, fusionLostFrames: 0,
  phase:'idle',referenceSpace:null,domOverlay:false,environmentBlendMode:null,frameCount:0,tracking:false,pose:null,
  scanPaused:false,originReset:false,wakeLockActive:false,wakeLockError:null,depthAvailable:false,depthUsage:null,depthDataFormat:null,depthType:null,
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
  /** The saved-scan restore finished and found no scan: a pending one-tap intent may start a Fresh scan now (issue #21). */
  scanLaunchReady?: boolean;
  onScanIntent?: (intent: ScanStartIntent) => boolean;
  onDeleteSavedScan?: () => void;
  onSessionActiveChange?: (active:boolean)=>void;
  /** `meta.relockTransform` is the B -> A transform found from known tags (issue #45) for a Continue that locked, else null. */
  onPointCloudReady?: (snapshot:ScanPointCloudSnapshot|null,meta?:{relockTransform:RelockTransform|null;relock?:RelockSummary|null})=>void;
  onCapabilitiesChange?: (capabilities:WebXrCapabilities)=>void;
  /** Restores (Continue, only when applicable) or clears the authoritative registration held by the parent. */
  onRegistrationChange?: (artifact:RegistrationArtifact|null)=>void;
  /** Intent of the scan being started; Fresh never restores the previous registration. */
  scanIntent?: ScanStartIntent;
  /** Identity of the durable base scan a Continue extends; the saved registration must be bound to it. */
  baseScanFrameId?: string|null;
  /** The tags saved with the base scan (project frame): a Continue is tied to them before any depth of the new session is used (issue #45). */
  getRelockBase?: ()=>TagPositionsArtifact|null;
  /** The base scan's measured points (project frame, metres) to show over the camera once the new session is tied to it (issue #45, gate 2). */
  getReferencePoints?: ()=>Float32Array|null;
  /** The saved scan's mesh (project frame triangle soup), when one exists; drawn over the camera after the tag lock. */
  getReferenceMesh?: ()=>Float32Array|null;
  /**
   * Provisional tag result of this AR session (unbound to any scan). final=true is sent exactly once
   * after Finish, always after onPointCloudReady, and is the signal that tag finalization is done.
   */
  onTagRegistrationResult?: (artifact:RegistrationArtifact|null,final:boolean)=>void;
  /** Tags confirmed from several viewpoints during this scan, for the result view (memory only; not part of the registration contract). */
  onTagAnchors?: (anchors:TagSensorAnchor[])=>void;
  /** The launch ended without an immersive session ever starting (preflight rejected, permission denied, unsupported): the intent never took effect. */
  onScanLaunchAborted?: ()=>void;
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
  scanLaunchReady=false,
  onScanIntent,
  onDeleteSavedScan,
  onSessionActiveChange,
  onPointCloudReady,
  onCapabilitiesChange,
  onRegistrationChange,
  scanIntent='fresh',
  baseScanFrameId=null,
  getRelockBase,
  getReferencePoints,
  getReferenceMesh,
  onTagRegistrationResult,
  onTagAnchors,
  onScanLaunchAborted,
}:XrRuntimePanelProps){
  const {locale}=useLocale();const t=useT();
  const runtime=useMemo(()=>new WebXrRuntime(),[]);const[capabilities,setCapabilities]=useState<WebXrCapabilities|null>(null);const[runtimeState,setRuntimeState]=useState<WebXrRuntimeState>(emptyRuntimeState);const[manualMode,setManualMode]=useState(false);const[aiStagingAllowed,setAiStagingAllowed]=useState(true);const[computePreparing,setComputePreparing]=useState(false);const[activeScanIntent,setActiveScanIntent]=useState<ScanStartIntent>('fresh');const[,setAiPriorRevision]=useState(0);const[,setCertifiedGeometryRevision]=useState(0);
  /* Issue #45, gate 1: a Continue scan is tied to the saved scan by known tags before any depth of the new session is used. The gate on the runtime keeps
     depth out while the lock is open; it opens on a lock and closes again when the frame moves. A saved scan with fewer than three confirmed tags cannot
     be re-located this way, and then the Continue works as before (overlap registration after the capture). */
  const[liveMeshMode,setLiveMeshModeState]=useState<LiveMeshMode>(()=>readLiveMeshMode());
  useEffect(()=>{runtime.setLiveMeshMode(liveMeshMode);},[runtime,liveMeshMode]);
  const[relock,setRelock]=useState<RelockState|null>(null);const[relockAligned,setRelockAligned]=useState(false);
  const relockBaseCountRef=useRef(0);const relockLastRef=useRef<{found:number;reason:string|null}>({found:0,reason:null});const relockArmedRef=useRef(false);const relockBaseRef=useRef<RelockBaseTag[]>([]);const relockTransformRef=useRef<RelockTransform|null>(null);const relockUsedRef=useRef<number[]>([]);
  const getRelockBaseRef=useRef(getRelockBase);getRelockBaseRef.current=getRelockBase;const getReferencePointsRef=useRef(getReferencePoints);getReferencePointsRef.current=getReferencePoints;const getReferenceMeshRef=useRef(getReferenceMesh);getReferenceMeshRef.current=getReferenceMesh;
  const placeReferenceMesh=(transform:RelockTransform|null):void=>{const mesh=getReferenceMeshRef.current?.()??null;runtime.setReferenceMesh(mesh===null||transform===null?null:buildReferenceMeshOverlay(mesh,transform));};
  const disarmRelock=():void=>{relockArmedRef.current=false;relockBaseRef.current=[];relockTransformRef.current=null;relockUsedRef.current=[];runtime.setReferenceOverlay(null);runtime.setReferenceMesh(null);setRelock(null);setRelockAligned(false);};
  const armRelock=():void=>{
    disarmRelock();
    if(scanIntentRef.current!=='continue')return;
    const base=relockBaseTags(getRelockBaseRef.current?.()??null,baseScanFrameIdRef.current);
    relockBaseCountRef.current=base.length;relockLastRef.current={found:0,reason:'need_more_tags'};
    if(base.length<RELOCK_MIN_TAGS){clientDiagnostics.info('XR_RELOCK_UNAVAILABLE',{baseTags:base.length});return;}
    relockBaseRef.current=base;relockArmedRef.current=true;setRelock({...INITIAL_RELOCK_STATE});
    clientDiagnostics.info('XR_RELOCK_START',{baseTags:base.map((tag)=>tag.tagId).join(',')});
  };
  const dropRelockLock=(event:string):void=>{relockTransformRef.current=null;relockUsedRef.current=[];runtime.setReferenceOverlay(null);runtime.setReferenceMesh(null);setRelockAligned(false);setRelock({...INITIAL_RELOCK_STATE});clientDiagnostics.warn(event,{});};
  const updateRelock=(observations:readonly TagObservation[]):void=>{
    if(!relockArmedRef.current)return;
    const fusion=fuseTagObservations(recentTagObservations(observations),{minObservations:RELOCK_MIN_OBSERVATIONS,minIndependentViews:RELOCK_MIN_VIEWS});
    const next=evaluateRelock(relockBaseRef.current,fusion);const held=relockTransformRef.current;relockLastRef.current={found:next.found,reason:next.reason};
    if(held!==null){
      if(relockLostByJump(relockUsedRef.current,fusion)||!relockStillHolds(held,next)){dropRelockLock('XR_RELOCK_LOST');return;}
      /* The same lock, now informed by more sightings: adopt the better fit (the merge is seeded with the latest) and move the overlay with it. */
      if(next.phase==='locked'&&next.transform!==null){
        const moved=Math.hypot(next.transform.tx-held.tx,next.transform.ty-held.ty,next.transform.tz-held.tz)*1000;const turned=Math.abs(next.transform.yaw-held.yaw);
        if(moved>2||turned>0.002){
          relockTransformRef.current=next.transform;relockUsedRef.current=next.usedTagIds;
          const reference=getReferencePointsRef.current?.()??null;runtime.setReferenceOverlay(reference===null?null:buildReferenceOverlay(reference,next.transform));placeReferenceMesh(next.transform);
          clientDiagnostics.info('XR_RELOCK_REFINED',{tags:next.usedTagIds.join(','),movedMm:Math.round(moved*10)/10,turnedDeg:Math.round(turned*5729.578)/100,rmsMm:Math.round((next.rmsMm??0)*10)/10});
        }
      }
      return;
    }
    if(next.phase==='locked'&&next.transform!==null){
      relockTransformRef.current=next.transform;relockUsedRef.current=next.usedTagIds;setRelockAligned(true);
      const reference=getReferencePointsRef.current?.()??null;
      runtime.setReferenceOverlay(reference===null?null:buildReferenceOverlay(reference,next.transform));placeReferenceMesh(next.transform);
      clientDiagnostics.info('XR_RELOCK_LOCKED',{tags:next.usedTagIds.join(','),rmsMm:Math.round((next.rmsMm??0)*10)/10,yawDeg:Math.round(next.transform.yaw*5729.578)/100,txM:Math.round(next.transform.tx*1000)/1000,tyM:Math.round(next.transform.ty*1000)/1000,tzM:Math.round(next.transform.tz*1000)/1000});
    }
    setRelock((previous)=>previous!==null&&previous.phase===next.phase&&previous.found===next.found&&previous.reason===next.reason?previous:next);
  };
  useEffect(()=>{
    /* A reset moves the origin: tags seen before it are in another frame, so the lock and what led to it are void (issue #36 / #45). */
    if(!runtimeState.originReset||!relockArmedRef.current)return;
    tagObservationsRef.current.clear();relockTransformRef.current=null;relockUsedRef.current=[];runtime.setReferenceOverlay(null);runtime.setReferenceMesh(null);setRelockAligned(false);setRelock({...INITIAL_RELOCK_STATE});clientDiagnostics.warn('XR_RELOCK_RESET',{});
  },[runtimeState.originReset]);
  useEffect(()=>{if((runtimeState.phase==='idle'||runtimeState.phase==='error')&&relockArmedRef.current)disarmRelock();},[runtimeState.phase]);
  useEffect(()=>{if(!relockAligned)return;const timer=window.setTimeout(()=>setRelockAligned(false),5000);return()=>window.clearTimeout(timer);},[relockAligned]);
  useEffect(()=>()=>{runtime.setReferenceOverlay(null);runtime.setReferenceMesh(null);},[runtime]);
  const[tagModeEnabled,setTagModeEnabled]=useState(false);const[tagGeometry,setTagGeometry]=useState<LocalizationSessionGeometry|null>(null);const[tagGeometryAt,setTagGeometryAt]=useState<number|null>(null);const[tagGeometryLoading,setTagGeometryLoading]=useState(false);const[tagModeError,setTagModeError]=useState<string|null>(null);const[tagFusion,setTagFusion]=useState<TagObservationFusion>({byTagId:{},rejectedCount:0});const[tagNotices,setTagNotices]=useState<TagScanNotice[]>([]);const tagConfirmedRef=useRef<number[]>([]);const tagJumpedRef=useRef<number[]>([]);const[tagRegistrationLabel,setTagRegistrationLabel]=useState<Parameters<typeof registrationOutcome>[0]>(null);
  const[manualCursor,setManualCursor]=useState<ManualCursorState>({u:.5,v:.5,phase:'scan-more',preview:null,distanceMeters:null});const manualPointer=useRef<ManualPointerState|null>(null);const lastPreviewUpdateMs=useRef(Number.NEGATIVE_INFINITY);const previousManualCount=useRef(0);const previousRuntimePhase=useRef<XrRuntimePhase>('idle');
  const onSessionActiveChangeRef=useRef(onSessionActiveChange),onPointCloudReadyRef=useRef(onPointCloudReady),onCapabilitiesChangeRef=useRef(onCapabilitiesChange),onRegistrationChangeRef=useRef(onRegistrationChange);
  const tagGeometryRef=useRef<{objectId:string;sessionId:string;geometry:LocalizationSessionGeometry;geometryComputedAtMs:number}|null>(null);const tagObservationsRef=useRef(new TagObservationStore());const tagClientRef=useRef<TagDetectorClient|null>(null);const tagFramesUnsubscribeRef=useRef<(()=>void)|null>(null);const tagTaskRef=useRef<Promise<void>|null>(null);const tagModeActiveRef=useRef(false);const tagAcceptPendingRef=useRef(false);const tagStopRef=useRef<(options?:{discard?:boolean})=>Promise<RegistrationArtifact|null>>(async()=>null);const tagGeometryEpoch=useRef(0);const tagLiveRef=useRef(new LiveTagTracker());const tagManualOffRef=useRef(false);const tagStatsRef=useRef({frames:0,busy:0,detections:0,poses:0,posesRejected:0,decodeMs:[] as number[],workerMs:[] as number[],fullPasses:0,coarsePasses:0,lastLogMs:0});const tagConfirmedKeyRef=useRef('');const tagPoseLoggedRef=useRef<Record<number,number>>({});
  const scanIntentRef=useRef<ScanStartIntent>(scanIntent);const baseScanFrameIdRef=useRef<string|null>(baseScanFrameId);baseScanFrameIdRef.current=baseScanFrameId;
  const onTagRegistrationResultRef=useRef(onTagRegistrationResult);const onTagAnchorsRef=useRef(onTagAnchors);onTagAnchorsRef.current=onTagAnchors;const sessionTagResultRef=useRef<RegistrationArtifact|null>(null);const tagSessionToken=useRef(0);
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
  /* The AI depth check is automatic. It is always armed; the frame it will analyse after the scan is only STAGED while the phone has headroom
     (scheduler pressure low, at most half of the live worker budget busy), so it never competes with the scan. Staging that already happened is kept. */
  useEffect(()=>{optionalMonocularPrior.setEnabled(true);},[]);
  useEffect(()=>{runtime.attach();tagMountedRef.current=true;bleMountedRef.current=true;const unsubscribe=runtime.subscribe(setRuntimeState);let cancelled=false;void probeWebXrCapabilities().then((result)=>{clientDiagnostics.info('XR_CAPABILITIES',result);void clientDiagnostics.flush();if(!cancelled){setCapabilities(result);onCapabilitiesChangeRef.current?.(result);}});return()=>{cancelled=true;tagMountedRef.current=false;bleMountedRef.current=false;tagGeometryEpoch.current+=1;bleIntentRef.current+=1;teardownTagModeForUnmount();const pointer=manualPointer.current;if(pointer?.timer!=null)window.clearTimeout(pointer.timer);manualPointer.current=null;unsubscribe();void runtime.detach();};},[runtime]);
  useEffect(()=>{tagGeometryEpoch.current+=1;tagGeometryRef.current=null;setTagGeometry(null);setTagGeometryAt(null);setTagGeometryLoading(false);setTagModeError(null);setTagRegistrationLabel(null);return()=>{tagGeometryEpoch.current+=1;};},[objectId,sessionId]);
  useEffect(()=>{const active=runtimeState.phase==='starting'||runtimeState.phase==='running'||runtimeState.phase==='stopping';onSessionActiveChangeRef.current?.(active);if(!active){setManualMode(false);manualPointer.current=null;}},[runtimeState.phase]);
  useEffect(()=>{
    const previous=previousRuntimePhase.current;const previousTarget=previousTagTargetRef.current;const targetChanged=previousTarget.objectId!==objectId||previousTarget.sessionId!==sessionId;
    previousRuntimePhase.current=runtimeState.phase;previousTagTargetRef.current={objectId,sessionId};
    const bootstrapping=runtimeState.phase==='starting'&&previous!=='starting';
    const retargeting=targetChanged&&(runtimeState.phase==='starting'||runtimeState.phase==='running');
    if(bootstrapping||retargeting){
      if(bootstrapping){onPointCloudReadyRef.current?.(null);armRelock();}
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
      const finishedRaw=runtimeState.phase==='ended'&&runtimeState.pointCount>0;
      /* Depth is gathered from the first frame of a Continue (points must appear at once). A Continue whose tags never locked is still handed over: the
         merge then falls back to the overlap search, and a rejection is reported with the reason, not hidden here. */
      const neverLocked=relockArmedRef.current&&relockTransformRef.current===null;
      const lockedForMerge=relockTransformRef.current;       /* read before disarming: the merge of this Continue is registered by it */
      const finished=finishedRaw;
      const relockSummary:RelockSummary|null=scanIntentRef.current==='continue'?{baseTags:relockBaseCountRef.current,found:relockLastRef.current.found,needed:RELOCK_MIN_TAGS,reason:relockLastRef.current.reason}:null;
      if(runtimeState.phase==='ended'&&neverLocked)clientDiagnostics.warn('XR_RELOCK_NEVER_LOCKED',{pointCount:runtimeState.pointCount});
      disarmRelock();
      /* MAX-403: the Finished scan is primary evidence and is handed over at once; tag finalization is a separate bounded stage. */
      if(finished&&tagMountedRef.current)onPointCloudReadyRef.current?.(runtime.getPointCloudSnapshot(),{relockTransform:lockedForMerge,relock:relockSummary});
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
    const fusion=fuseTagObservations(tagObservationsRef.current.getObservations(),{minObservations:5,minIndependentViews:3});
    tagObservationsRef.current.recordOutliers(fusion.rejectedCount);setTagFusion(fusion);tagLiveRef.current.setConfirmed(confirmedTagIds(fusion));
    onTagAnchorsRef.current?.(tagSensorAnchorsFromFusion(fusion));
    const bound=tagGeometryRef.current;
    /* Live recognition needs no recording. Registration does: without frozen geometry for this scan target the observations are kept in memory only, and that is not an operator-facing error. */
    /* Live recognition needs no recording; registration does. Without frozen geometry for this scan target nothing is registered or persisted here (the
       registration contract requires a geometry generation). The tags confirmed during the scan still reach the result view through onTagAnchors above. */
    if(bound===null||bound.objectId!==objectId||bound.sessionId!==sessionId){clientDiagnostics.info('XR_TAG_REGISTRATION_SKIPPED',{reason:'no_localization_geometry',confirmed:confirmedTagIds(fusion).join(','),observations:tagObservationsRef.current.retainedCount});void clientDiagnostics.flush();return null;}
    clientDiagnostics.info('XR_TAG_OBSERVATION_STORE_FINALIZED',{retained:tagObservationsRef.current.retainedCount,rejected:tagObservationsRef.current.rejectedCount,outliers:tagObservationsRef.current.outlierCount,evicted:tagObservationsRef.current.evictedCount});void clientDiagnostics.flush();
    const result=registerSensorToScan({tags:fusion.byTagId,geometry:bound.geometry,geometryComputedAtMs:bound.geometryComputedAtMs});
    /* A REMOTE_ONLY session has no MAIN tag to fit against: the geophones' own tag centres are their positions (no offset). */
    const geophoneBlock=bound.geometry.provenance?.topology_mode==='remote_only'?geophonePositionsRegistrationBlock(geophonePositionsFromTags({tags:fusion.byTagId})):null;
    const artifact=registrationArtifactFromResult(result,fusion,defaultRegistrationTagSizes(),bound.geometryComputedAtMs,Date.now(),null,undefined,geophoneBlock);
    /* Persistence is a separate stage owned by the parent after the scan commit (it needs the committed scan id). */
    try{encodeRegistrationArtifact(artifact);}catch{setTagModeError('Registration artifact was not accepted.');return null;}
    setTagRegistrationLabel(geophoneBlock===null?result:{...result,geophone_positions:geophoneBlock});setTagModeError(null);
    return artifact;
  };

  tagStopRef.current=stopTagMode;
  const startTagMode=():void=>{
    const captureError=runtime.setTagCaptureEnabled(true);if(captureError!==null){setTagModeError(captureError);return;}
    const client=new TagDetectorClient();
    if(!client.workerBacked){client.dispose();runtime.setTagCaptureEnabled(false);setTagModeError('AprilTag detector worker could not start.');return;}
    tagObservationsRef.current.clear();tagLiveRef.current.reset();tagStatsRef.current={frames:0,busy:0,detections:0,poses:0,posesRejected:0,decodeMs:[],workerMs:[],fullPasses:0,coarsePasses:0,lastLogMs:performance.now()};tagConfirmedKeyRef.current='';tagPoseLoggedRef.current={};tagAcceptPendingRef.current=false;setTagFusion({byTagId:{},rejectedCount:0});setTagNotices([]);tagConfirmedRef.current=[];setTagRegistrationLabel(null);setTagModeError(null);
    clientDiagnostics.info('XR_TAG_LIVE_START',{recording:objectId!==null&&sessionId!==null,geometryBound:tagGeometryRef.current!==null});
    const token=++tagSessionToken.current;
    tagClientRef.current=client;tagModeActiveRef.current=true;setTagModeEnabled(true);
    tagFramesUnsubscribeRef.current=runtime.subscribeTagFrames((frame)=>{
      if(!tagModeActiveRef.current)return;
      const stats=tagStatsRef.current;
      if(client.busy){stats.busy+=1;return;}
      const decodeStartedMs=performance.now();stats.frames+=1;
      // The capture marks its fresh RGBA buffer as owned; detect transfers that buffer to the worker.
      const task=client.detect(frame).then((detected)=>{
        if(detected===null||(!tagModeActiveRef.current&&!tagAcceptPendingRef.current)||tagSessionToken.current!==token||!tagMountedRef.current)return;
        const nowMs=performance.now();stats.decodeMs.push(nowMs-decodeStartedMs);runtime.reportTagDecode(nowMs-decodeStartedMs);if(stats.decodeMs.length>120)stats.decodeMs.shift();
        if(detected.workerMs!==undefined){stats.workerMs.push(detected.workerMs);if(stats.workerMs.length>120)stats.workerMs.shift();}if(detected.pass==='full')stats.fullPasses+=1;else if(detected.pass==='coarse')stats.coarsePasses+=1;
        for(const detection of detected.detections){
          const size=defaultRegistrationTagSizes()[String(detection.tagId) as '0'|'1'|'2'|'3'|'4'];
          const pose=estimateTagPose({corners:detection.corners,projectionMatrix:detected.projectionMatrix,worldFromView:detected.worldFromView,tagSizeM:size,capturedAtMs:detected.capturedAtMs,cameraPosition:detected.cameraPosition,cameraForward:detected.cameraForward});
          stats.detections+=1;
          const firstSeen=tagLiveRef.current.ingest({tagId:detection.tagId,corners:detection.corners,pose,sizeM:size,capturedAtMs:detected.capturedAtMs});
          if(firstSeen)clientDiagnostics.info('XR_TAG_DETECTED',{tagId:detection.tagId,node:tagNodeForId(detection.tagId),hamming:detection.hammingDistance,pose:pose.ok,distanceM:pose.ok?Math.round(pose.distanceM*1000)/1000:null,reproj:pose.ok?Math.round(pose.reprojectionErrorNorm*10000)/10000:null,reason:pose.ok?null:pose.reason});
          if(pose.ok&&nowMs-(tagPoseLoggedRef.current[detection.tagId]??-1e9)>=2000){tagPoseLoggedRef.current[detection.tagId]=nowMs;const r=(v:number)=>Math.round(v*1000)/1000;clientDiagnostics.info('XR_TAG_POSE',{tagId:detection.tagId,node:tagNodeForId(detection.tagId),x:r(pose.positionScan[0]),y:r(pose.positionScan[1]),z:r(pose.positionScan[2]),dist:r(pose.distanceM),reproj:Math.round(pose.reprojectionErrorNorm*10000)/10000,cam:detected.cameraPosition.map(r).join(','),pass:detected.pass??null,t:Math.round(detected.capturedAtMs)});}
          if(pose.ok){stats.poses+=1;tagObservationsRef.current.insert({tagId:detection.tagId,positionScan:pose.positionScan,capturedAtMs:pose.capturedAtMs,cameraPosition:pose.cameraPosition,cameraForward:pose.cameraForward});}
          else{stats.posesRejected+=1;tagObservationsRef.current.recordRejected();}
        }
        if(nowMs-stats.lastLogMs>=5000){
          const sorted=[...stats.decodeMs].sort((a,b)=>a-b);const pick=(q:number)=>sorted.length===0?null:Math.round(sorted[Math.min(sorted.length-1,Math.ceil(q*sorted.length)-1)]);const sortedWorker=[...stats.workerMs].sort((a,b)=>a-b);const pickWorker=(q:number)=>sortedWorker.length===0?null:Math.round(sortedWorker[Math.min(sortedWorker.length-1,Math.ceil(q*sortedWorker.length)-1)]);
          clientDiagnostics.info('XR_TAG_STATS',{frames:stats.frames,busyDrops:stats.busy,detections:stats.detections,poses:stats.poses,posesRejected:stats.posesRejected,decodeP50:pick(0.5),decodeP95:pick(0.95),workerP50:pickWorker(0.5),workerP95:pickWorker(0.95),fullPasses:stats.fullPasses,coarsePasses:stats.coarsePasses,outliers:tagLiveRef.current.outliersRejected,live:tagLiveRef.current.states().map((state)=>`${state.node}:${state.status}`).join(',')});
          stats.lastLogMs=nowMs;
        }
        if(detected.detections.length===0)return;
        const fusion=fuseTagObservations(tagObservationsRef.current.getObservations(),{minObservations:5,minIndependentViews:3});
        if(relockArmedRef.current)updateRelock(tagObservationsRef.current.getObservations());    /* a Fresh scan pays nothing for the lock */
        const confirmedNow=confirmedTagIds(fusion);tagLiveRef.current.setConfirmed(confirmedNow);
        const confirmedKey=`${confirmedNow.join(',')}|${frameJumpTagIds(fusion).join(',')}`;if(confirmedKey!==tagConfirmedKeyRef.current){tagConfirmedKeyRef.current=confirmedKey;setTagFusion(fusion);}
      }).catch((reason)=>{if(tagModeActiveRef.current)setTagModeError(`AprilTag detection failed: ${reason instanceof Error?reason.message:String(reason)}`);});
      tagTaskRef.current=task;void task.finally(()=>{if(tagTaskRef.current===task)tagTaskRef.current=null;});
    });
  };

  const toggleTagMode=():void=>{
    if(!tagModeActiveRef.current){tagManualOffRef.current=false;startTagMode();return;}
    tagManualOffRef.current=true;
    void stopTagMode().then((artifact)=>{if(artifact===null||!tagMountedRef.current)return;sessionTagResultRef.current=artifact;onTagRegistrationResultRef.current?.(artifact,false);});
  };
  /* Announcements fade by themselves: nothing about the tags stays on the camera view. */
  useEffect(()=>{if(tagNotices.length===0)return;const timer=window.setTimeout(()=>setTagNotices([]),7000);return()=>window.clearTimeout(timer);},[tagNotices]);
  /* A device whose tag has just been confirmed by the live scan is announced, with the pairs whose distance is recalculated from the scan. */
  useEffect(()=>{const added=newTagScanNotices(tagConfirmedRef.current,tagFusion);tagConfirmedRef.current=confirmedTagIds(tagFusion);const jumps=newFrameJumpNotices(tagJumpedRef.current,tagFusion);tagJumpedRef.current=frameJumpTagIds(tagFusion);for(const jump of jumps)clientDiagnostics.warn('XR_TAG_FRAME_JUMP',{tagId:jump.tagId,node:jump.node,separationM:Math.round((jump.separationM??0)*1000)/1000});const shown=[...added,...jumps];if(shown.length>0)setTagNotices((previous)=>mergeTagNotices(previous,shown));},[tagFusion]);
  /* Live recognition needs only the phone camera path. A recording and its localization geometry are required later, for registration, not for seeing the tags. */
  const tagUnavailableReason=capabilities?.rawXrRgbApi===false?t('xr.tag.camera_unavailable'):capabilities!==null&&capabilities.tagReadbackMode!=='nonblocking'?t('xr.tag.readback_unavailable'):null;
  /* Tag recognition starts by itself with the scan; the Tags button is only a diagnostic switch. */
  useEffect(()=>{
    if(runtimeState.phase==='starting')tagManualOffRef.current=false;
    if(runtimeState.phase!=='running'||tagModeActiveRef.current||tagManualOffRef.current||capabilities===null||tagUnavailableReason!==null)return;
    startTagMode();
  },[runtimeState.phase,capabilities,tagUnavailableReason]);
  /* AI check staging gate: stage the post-scan frame only while the phone has headroom (pressure low, at most half of the live worker budget busy). */
  useEffect(()=>{
    if(runtimeState.phase!=='running'){optionalMonocularPrior.setStagingAllowed(true);setAiStagingAllowed(true);return;}
    const evaluate=():void=>{
      const snapshot=liveComputeScheduler.snapshot();const budget=snapshot.effectiveBudget??snapshot.maximumBudget??0;
      const allowed=snapshot.pressure==='low'&&!snapshot.degraded&&budget>0&&snapshot.active<=budget*0.5;
      if(optionalMonocularPrior.isStagingAllowed===allowed)return;
      optionalMonocularPrior.setStagingAllowed(allowed);setAiStagingAllowed(allowed);
      clientDiagnostics.info('XR_AI_STAGING',{allowed,pressure:snapshot.pressure,degraded:snapshot.degraded,active:snapshot.active,budget,staged:optionalMonocularPrior.hasStagedPostScanFrame,status:optionalMonocularPrior.status});
    };
    evaluate();const timer=window.setInterval(evaluate,1000);return()=>window.clearInterval(timer);
  },[runtimeState.phase]);
  /* Developer telemetry. Everything the camera overlay used to show (counts, motion, CPU), what is switched on/off and the frame it happened on, and
     the load figures go to the log instead of the screen. Browsers expose no GPU utilisation: frame pacing, long frames and worker pressure stand in for it. */
  const runtimeStateRef=useRef(runtimeState);runtimeStateRef.current=runtimeState;
  const devFlagsRef=useRef<Record<string,string>>({});
  useEffect(()=>{
    if(runtimeState.phase!=='running'){devFlagsRef.current={};return;}
    const flags:Record<string,string>={paused:String(runtimeState.scanPaused),tracking:String(runtimeState.tracking),depth:String(runtimeState.depthAvailable),rgb:String(runtimeState.rgbGuidanceAvailable),
      guide:String(runtimeState.coverageGuideActive),motion:String(runtimeState.motionState),tags:String(tagModeEnabled),ai:String(runtimeState.monocularPriorStatus),aiStaging:String(aiStagingAllowed)};
    const previous=devFlagsRef.current;const changed=Object.keys(flags).filter((key)=>previous[key]!==flags[key]);
    if(changed.length>0){clientDiagnostics.info('XR_STATE_CHANGE',{frame:runtimeState.frameCount,changed:changed.map((key)=>`${key}:${previous[key]??'-'}>${flags[key]}`).join(' ')});devFlagsRef.current=flags;}
  },[runtimeState.phase,runtimeState.scanPaused,runtimeState.tracking,runtimeState.depthAvailable,runtimeState.rgbGuidanceAvailable,runtimeState.coverageGuideActive,runtimeState.motionState,runtimeState.monocularPriorStatus,tagModeEnabled,aiStagingAllowed]);
  useEffect(()=>{
    if(runtimeState.phase!=='running'||runtimeState.keyframeCount===0)return;
    clientDiagnostics.info('XR_KEYFRAME',{n:runtimeState.keyframeCount,frame:runtimeState.frameCount,depthFrame:runtimeState.depthFrameCount,integrated:runtimeState.sampledDepthFrames,points:runtimeState.pointCount,polish:runtimeState.polishFrameCount});
  },[runtimeState.keyframeCount]);
  useEffect(()=>{
    if(runtimeState.phase!=='running')return;
    const r=(value:number,digits=2)=>Number.isFinite(value)?Math.round(value*10**digits)/10**digits:null;
    const timer=window.setInterval(()=>{
      const s=runtimeStateRef.current;const sch=liveComputeScheduler.snapshot();const p=s.performance;const memory=(performance as unknown as {memory?:{usedJSHeapSize:number}}).memory;
      clientDiagnostics.info('XR_VIEWPORT_STATE',{f:s.frameCount,pause:s.scanPaused,trk:s.tracking,mot:s.motionState,mq:r(s.motionQuality),v:r(s.linearSpeedMetersPerSecond),w:r(s.angularSpeedRadiansPerSecond),acc:r(s.linearAccelerationMetersPerSecond2),
        dep:s.depthWidth&&s.depthHeight?`${s.depthWidth}x${s.depthHeight}`:'-',df:s.depthFrameCount,int:s.sampledDepthFrames,rej:s.motionRejectedFrames,pts:s.pointCount,ret:s.retainedPointCount,kf:s.keyframeCount,pol:s.polishFrameCount,
        cells:`${s.coverageReadyCells}/${s.coverageVisibleCells}/${s.coverageTotalCells}`,weak:s.coverageWeakPoints,imp:s.coverageImprovingPoints,stb:s.coverageStablePoints,
        rgb:s.rgbGuidanceAvailable?`${s.rgbGuidanceWidth}x${s.rgbGuidanceHeight}`:'off',vis:s.visualEvidenceFrames,light:s.lightingEstimateAvailable,wake:s.wakeLockActive,man:`${s.manualPointCount}/${s.manualEdgeCount}`});
      clientDiagnostics.info('XR_PERF_STATE',{f:s.frameCount,p50:r(p.frameDeltaP50Ms,1),p95:r(p.frameDeltaP95Ms,1),max:r(p.frameDeltaMaxMs,1),work95:r(p.frameWorkP95Ms,1),depth95:r(p.depthAccessP95Ms,1),fus50:r(p.fusionP50Ms,1),fus95:r(p.fusionP95Ms,1),
        cov95:r(p.coverageP95Ms,1),l33:p.longFrames33Ms,l50:p.longFrames50Ms,n:p.sampleCount,press:p.pressure,cpu:`${sch.active}/${sch.effectiveBudget}/${sch.maximumBudget}`,sched:sch.pressure,degr:sch.degraded,
        struct:sch.structural,vis:sch.visual,covInt:p.adaptiveCoverageIntervalMs,covPts:p.adaptiveCoveragePointBudget,heapMb:memory===undefined?null:r(memory.usedJSHeapSize/1048576,1)});
    },5000);
    return()=>window.clearInterval(timer);
  },[runtimeState.phase]);
  /* A tag that stops decoding becomes LOST (its anchored outline stays, dimmed). */
  useEffect(()=>{
    if(!tagModeEnabled)return;
    const timer=window.setInterval(()=>{for(const tagId of tagLiveRef.current.tick(performance.now()))clientDiagnostics.info('XR_TAG_LOST',{tagId,node:tagNodeForId(tagId)});},500);
    return()=>window.clearInterval(timer);
  },[tagModeEnabled]);

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
    }finally{
      setComputePreparing(false);
      const phase=runtime.getState().phase;
      if(phase!=='starting'&&phase!=='running'&&phase!=='stopping')onScanLaunchAborted?.();
    }
  };

  /* One-tap start: a tap on "3D scan" left a single-use intent (scanLaunchIntent.ts). It is honoured only for a session with no saved scan, while the
     browser activation of that tap is still live; otherwise the Start button below stays the way in. A saved scan drops the intent: it opens as viewer. */
  useEffect(()=>{
    if(savedScanAvailable){clearScanLaunchIntent();return;}
    if(!scanLaunchReady||!canStart||objectId===null)return;
    /* Deferred by one task: a development Strict Mode replay mounts, cleans up and mounts again within the same task. Starting inside the first run
       asked for the session, then the cleanup detached the runtime and the session was ended as obsolete a second later (phone run 2026-11-21 23:05:
       XR_SESSION_STARTED then XR_SESSION_ENDED, and the operator had to press Start again). The cleanup cancels the timer, so only the surviving mount starts. */
    const timer=window.setTimeout(()=>{
      if(!consumeScanLaunchIntent(objectId,sessionId??SCAN_LAUNCH_STAGING_SESSION))return;
      clientDiagnostics.info('XR_ONE_TAP_LAUNCH',{objectId,sessionId});
      void startPreparedScan('fresh');
    },0);
    return()=>window.clearTimeout(timer);
  },[scanLaunchReady,canStart,savedScanAvailable,objectId,sessionId]);

  const continuationSampleReady=runtimeState.pointCount>=120;

  return <>
    <section className="xr-runtime-card xr-runtime-card-compact" aria-label={t('xr.title')}>
      <div className="xr-runtime-heading"><div><div className="panel-title">{t('xr.heading')}</div></div><span className={`xr-phase ${qualityClass(runtimeState)}`}>{phaseLabel(runtimeState,locale)}</span></div>
      <div className="xr-compact-status"><span className={capabilities?.immersiveAr?'good-text':'bad-text'}>WebXR {capabilities?.immersiveAr?'✓':capabilities===null?t('xr.value.checking'):'—'}</span><span className={runtimeState.depthAvailable?'good-text':''}>{t('xr.label.depth')} {runtimeState.depthAvailable?'✓':runtimeState.phase==='idle'?t('xr.value.on_start'):'—'}</span><span className={runtimeState.rgbGuidanceAvailable?'good-text':''}>{t('xr.label.rgb')} {runtimeState.rgbGuidanceAvailable?'✓':capabilities?.rawXrRgbApi?t('xr.value.on_start'):'—'}</span><span className={runtimeState.wakeLockActive?'good-text':''}>{t('xr.label.screen')} {runtimeState.wakeLockActive?t('xr.value.awake'):capabilities?.wakeLockApi?t('xr.value.wake_ready'):t('xr.value.wake_lock_off')}</span></div>
      <div className="xr-runtime-controls xr-runtime-controls-compact">
        {savedScanAvailable ? <>
          <button type="button" className="xr-start-button" disabled={!canStart||savedScanBusy} onClick={()=>void startPreparedScan('continue')}>{computePreparing?t('xr.starting_ar'):t('xr.continue_scan')}</button>
          <button type="button" className="xr-undo-button" disabled={!canStart||savedScanBusy} onClick={()=>void startPreparedScan('fresh')}>{t('xr.scan_again')}</button>
          {onDeleteSavedScan!==undefined&&<button type="button" className="xr-undo-button" disabled={busy||savedScanBusy} onClick={onDeleteSavedScan}>{t('xr.delete_saved_scan')}</button>}
          <div className="xr-finish-explainer">{t('xr.continue_description')}</div>
        </> : <>
          <button type="button" className="xr-start-button" disabled={!canStart||savedScanBusy} onClick={()=>void startPreparedScan('fresh')}>{computePreparing?t('xr.starting_ar'):runtimeState.phase==='interrupted'?t('xr.discard_partial_start'):t('xr.start_scan')}</button>
        </>}
        <label className="xr-live-mesh-setting">{t('xr.live_mesh.label')}
          <select value={liveMeshMode} onChange={(event)=>{const next=event.target.value as LiveMeshMode;if(!LIVE_MESH_MODES.includes(next))return;setLiveMeshModeState(next);writeLiveMeshMode(next);clientDiagnostics.info('XR_LIVE_MESH_MODE',{mode:next});}}>
            {LIVE_MESH_MODES.map((mode)=><option key={mode} value={mode}>{t(`xr.live_mesh.${mode}` as 'xr.live_mesh.auto')}</option>)}
          </select>
        </label>
        {bleSupport==='supported'?<button type="button" className={bleActive?'xr-undo-button active':'xr-undo-button'} aria-pressed={bleActive} disabled={runtimeState.phase!=='running'} onClick={()=>void toggleBle()} title={t('xr.ble_hints_title')}>{bleActive?t('xr.ble_hints_on'):t('xr.ble_hints_off')}</button>:<div className="xr-finish-explainer">{describeBleRssiSupport(bleSupport,locale)}</div>}
      </div>
      {runtimeState.phase==='interrupted'&&<div className="xr-runtime-warning" role="status">{t('xr.session_interrupted')}</div>}
      {runtimeState.depthError!==null&&<div className="xr-runtime-warning" role="status">{formatErrorText(locale,runtimeState.depthError)}</div>}{runtimeState.error!==null&&<div className="xr-runtime-error" role="alert">{formatErrorText(locale,runtimeState.error)}</div>}
      <details className="xr-technical-details"><summary>{t('scan.details.technical')}</summary><dl className="xr-runtime-values">
        <div><dt>{t('xr.detail.depth_type')}</dt><dd>{runtimeState.depthType??'--'}</dd></div><div><dt>{t('xr.detail.depth_usage')}</dt><dd>{runtimeState.depthUsage??'--'}</dd></div><div><dt>{t('xr.detail.depth_format')}</dt><dd>{runtimeState.depthDataFormat??'--'}</dd></div><div><dt>{t('xr.detail.depth_size')}</dt><dd>{runtimeState.depthWidth&&runtimeState.depthHeight?`${formatNumber(locale,runtimeState.depthWidth)} × ${formatNumber(locale,runtimeState.depthHeight)}`:'--'}</dd></div>
        <div><dt>{t('xr.detail.raw_rgb')}</dt><dd>{capabilities?.rawXrRgbApi?t('scan.compute.available'):'--'}</dd></div><div><dt>{t('xr.detail.rgb_guidance')}</dt><dd>{runtimeState.rgbGuidanceAvailable?`${formatNumber(locale,runtimeState.rgbGuidanceWidth??0)} × ${formatNumber(locale,runtimeState.rgbGuidanceHeight??0)}`:'--'}</dd></div><div><dt>{t('xr.detail.video_inputs')}</dt><dd>{capabilities?.videoInputsEnumerated?formatNumber(locale,capabilities.videoInputCount??0):capabilities?.videoInputEnumeration?t('xr.value.not_exposed'):'--'}</dd></div><div><dt>{t('xr.detail.hardware_depth_sensor')}</dt><dd>{capabilities?.hardwareDepthSensor?.toUpperCase()??t('xr.value.unknown')} · {t('xr.hardware_depth_not_inferred')}</dd></div><div><dt>{t('xr.detail.visual_evidence_frames')}</dt><dd>{formatNumber(locale,runtimeState.visualEvidenceFrames)}</dd></div>
        <div><dt>{t('xr.detail.stable_points')}</dt><dd>{formatNumber(locale,runtimeState.pointCount)}</dd></div><div><dt>{t('xr.detail.retained_hypotheses')}</dt><dd>{formatNumber(locale,runtimeState.retainedPointCount)}</dd></div><div><dt>{t('scan.details.keyframes')}</dt><dd>{formatNumber(locale,runtimeState.keyframeCount)}</dd></div><div><dt>{t('scan.details.polish_frames')}</dt><dd>{formatNumber(locale,runtimeState.polishFrameCount)}</dd></div><div><dt>{t('xr.detail.motion_rejects')}</dt><dd>{formatNumber(locale,runtimeState.motionRejectedFrames)}</dd></div>
        <div><dt>{t('xr.detail.ready_visible_cells')}</dt><dd>{readyVisible}</dd></div><div><dt>{t('xr.detail.coverage_confidence')}</dt><dd>{formatPercent(locale,runtimeState.coverageAverageConfidence)}</dd></div><div><dt>{t('xr.detail.weak_points')}</dt><dd>{formatNumber(locale,runtimeState.coverageWeakPoints)}</dd></div><div><dt>{t('xr.detail.improving_points')}</dt><dd>{formatNumber(locale,runtimeState.coverageImprovingPoints)}</dd></div><div><dt>{t('xr.detail.stable_guide_points')}</dt><dd>{formatNumber(locale,runtimeState.coverageStablePoints)}</dd></div>
        <div><dt>{t('xr.detail.worker_api')}</dt><dd>{capabilities?.workerApi?t('scan.compute.available'):'--'}</dd></div><div><dt>{t('xr.detail.shared_memory')}</dt><dd>{capabilities?.sharedMemory?t('scan.compute.available'):'--'}</dd></div><div><dt>{t('xr.detail.wasm_threads')}</dt><dd>{capabilities?.wasmThreads?t('scan.compute.available'):'--'}</dd></div><div><dt>WebGPU</dt><dd>{capabilities?.webGpu?t('scan.compute.available'):'--'}</dd></div><div><dt>{t('xr.detail.live_cpu_workers')}</dt><dd>{computeLabel} · {pressureLabel(computeScheduler.pressure,t)}{computeScheduler.degraded?` ${t('xr.value.throttled')}`:''}</dd></div><div><dt>{t('xr.detail.active_heavy_jobs')}</dt><dd>{formatNumber(locale,computeScheduler.active)} · S{formatNumber(locale,computeScheduler.structural)} / V{formatNumber(locale,computeScheduler.visual)}</dd></div><div><dt>{t('xr.detail.manual_corners')}</dt><dd>{formatNumber(locale,runtimeState.manualPointCount)}</dd></div><div><dt>{t('xr.detail.confirmed_edges')}</dt><dd>{formatNumber(locale,runtimeState.manualEdgeCount)}</dd></div><div><dt>{t('xr.detail.frame_p95')}</dt><dd>{formatNumber(locale,runtimeState.performance.frameDeltaP95Ms,{minimumFractionDigits:1,maximumFractionDigits:1})} ms</dd></div><div><dt>{t('xr.detail.fusion_p50_p95')}</dt><dd>{formatNumber(locale,runtimeState.performance.fusionP50Ms,{minimumFractionDigits:1,maximumFractionDigits:1})} / {formatNumber(locale,runtimeState.performance.fusionP95Ms,{minimumFractionDigits:1,maximumFractionDigits:1})} ms</dd></div><div><dt>{t('xr.detail.coverage_p95')}</dt><dd>{formatNumber(locale,runtimeState.performance.coverageP95Ms,{minimumFractionDigits:1,maximumFractionDigits:1})} ms</dd></div><div><dt>{t('xr.detail.long_frames')}</dt><dd>{formatNumber(locale,runtimeState.performance.longFrames33Ms)} / {formatNumber(locale,runtimeState.performance.longFrames50Ms)} ms</dd></div><div><dt>{t('xr.detail.live_render_pressure')}</dt><dd>{pressureLabel(runtimeState.performance.pressure,t)} · {formatNumber(locale,runtimeState.performance.adaptiveCoverageIntervalMs)} ms</dd></div>
        <div><dt>{t('xr.detail.linear_speed')}</dt><dd>{formatNumber(locale,runtimeState.linearSpeedMetersPerSecond,{minimumFractionDigits:2,maximumFractionDigits:2})} m/s</dd></div><div><dt>{t('xr.detail.angular_speed')}</dt><dd>{formatNumber(locale,runtimeState.angularSpeedRadiansPerSecond,{minimumFractionDigits:2,maximumFractionDigits:2})} rad/s</dd></div><div><dt>{t('xr.detail.acceleration')}</dt><dd>{formatNumber(locale,runtimeState.linearAccelerationMetersPerSecond2,{minimumFractionDigits:2,maximumFractionDigits:2})} m/s²</dd></div><div><dt>{t('xr.detail.motion_quality')}</dt><dd>{formatPercent(locale,runtimeState.motionQuality)}</dd></div><div><dt>{t('xr.detail.lighting_estimate')}</dt><dd>{runtimeState.lightingEstimateAvailable?t('scan.compute.available'):'--'}</dd></div><div><dt>{t('xr.detail.primary_light')}</dt><dd>{runtimeState.primaryLightIntensity===null?'--':formatNumber(locale,runtimeState.primaryLightIntensity,{minimumFractionDigits:3,maximumFractionDigits:3})}</dd></div><div><dt>{t('xr.detail.wake_lock')}</dt><dd>{runtimeState.wakeLockActive?t('xr.value.active'):runtimeState.wakeLockError??'--'}</dd></div>
        <div><dt>{t('xr.tag.tags')}</dt><dd><button type="button" className={tagModeEnabled?'xr-manual-toggle active':'xr-manual-toggle'} aria-pressed={tagModeEnabled} disabled={!running||(!tagModeEnabled&&tagUnavailableReason!==null)} title={tagModeEnabled?t('xr.tag.tags_on'):t('xr.tag.capture_title')} onClick={toggleTagMode}>{tagModeEnabled?t('xr.tag.tags_on'):t('xr.tag.tags')}</button></dd></div>
        <div><dt>{t('xr.detail.tag_readback')}</dt><dd>{capabilities?.tagReadbackMode==='nonblocking'?t('xr.value.tag_readback_nonblocking'):capabilities?.tagReadbackMode==='synchronous'?t('xr.value.tag_readback_synchronous'):capabilities?.tagReadbackMode==='unavailable'?t('xr.value.tag_readback_unavailable'):'--'}</dd></div>
      </dl></details>
    </section>
    <StructuralGuideOverlay runtime={runtime} runtimeState={runtimeState} interactionEnabled={running&&!manualMode}/>
    <LiveTagOverlay runtime={runtime} tracker={tagLiveRef.current} active={running&&tagModeEnabled}/>
    <div className="xr-immersive-overlay" aria-live="polite">
      {running&&bleActive&&<BleRoughPlacementHud estimates={bleEstimates} pose={runtimeState.pose}/>}
      <div className="xr-overlay-card xr-overlay-card-compact">{/* developer figures (counts, motion, CPU) are in the log, not over the camera */}
        {running&&activeScanIntent==='continue'&&<div className="xr-overlay-hint">{t('xr.overlay.continue_hint',{sample:continuationSampleReady?t('xr.overlay.continue_enough'):t('xr.overlay.registration_sample',{count:formatNumber(locale,runtimeState.pointCount)})})}</div>}
        {relock!==null&&relock.phase==='relocalizing'&&<div className="xr-overlay-hint" role="status" data-relock="searching" data-relock-found={relock.found}>{t('xr.relock.find',{found:Math.min(relock.found,relock.needed),needed:relock.needed})}{relock.found>=relock.needed&&relock.reason!==null&&relock.reason!=='need_more_tags'?` · ${t(`xr.relock.reason.${relock.reason}` as 'xr.relock.reason.collinear')}`:''}</div>}
        {relockAligned&&<div className="xr-overlay-hint" role="status" data-relock="aligned">{t('xr.relock.aligned')}</div>}
        {runtimeState.phase==='running'&&runtimeState.fusionCatchingUp&&<div className="xr-overlay-hint" role="status" data-fusion="catching-up">{t('xr.fusion.catching_up')}</div>}
        {runtimeState.phase==='running'&&runtimeState.fusionLostFrames>0&&<div className="xr-overlay-hint" role="alert" data-fusion="lost">{t('xr.fusion.lost_frames',{count:runtimeState.fusionLostFrames})}</div>}
        {runtimeState.originReset&&<div className="xr-overlay-warning" role="alert" data-origin-reset="true">{t('xr.origin_reset')}</div>}{runtimeState.scanPaused&&!runtimeState.originReset&&<div className="xr-overlay-warning">{t('xr.manual.paused')}</div>}{runtimeState.motionState==='too-fast'&&<div className="xr-overlay-warning">{t('xr.manual.move_slower')}</div>}{(runtimeState.motionState==='stationary'||runtimeState.motionState==='redundant')&&<div className="xr-overlay-hint">{t('xr.manual.move_viewpoint')}</div>}{runtimeState.manualPointErrorKey!==null&&<div className="xr-overlay-warning" role="status">{t(runtimeState.manualPointErrorKey)}</div>}{runtimeState.manualPointErrorKey===null&&runtimeState.manualPointError!==null&&<div className="xr-overlay-warning">{manualPlacementErrorLabel(runtimeState.manualPointError,locale)}</div>}
        {running&&!tagModeEnabled&&tagUnavailableReason!==null&&<div className="xr-overlay-hint">{t('xr.tag.unavailable',{reason:tagUnavailableReason})}</div>}
        {tagModeEnabled&&tagNotices.length>0&&<div className="xr-tag-notices" role="status" aria-live="polite">{tagNotices.map((notice)=><div key={`${notice.kind??'ok'}-${notice.tagId}`} className={notice.kind==='jump'?'xr-overlay-warning xr-tag-notice':'xr-overlay-hint xr-tag-notice'} data-node={notice.node} data-frame-jump={notice.kind==='jump'?'true':undefined}>{tagScanNoticeText(notice,locale)}</div>)}</div>}
        {tagModeError!==null&&<div className="xr-overlay-warning" role="status">{tagModeErrorLabel(tagModeError,locale)}</div>}
        {staleRegistrationReason!==null&&<div className="xr-overlay-warning" role="status">{t('xr.tag.registration.stale',{reason:t(REGISTRATION_INAPPLICABLE_KEYS[staleRegistrationReason])})}</div>}
        {tagRegistrationLabel!==null&&<div className="xr-overlay-hint" role="status">{registrationOutcome(tagRegistrationLabel,locale)}</div>}
        <button className="xr-finish-button" type="button" disabled={!running} onClick={()=>void runtime.stop()}>{t('xr.finish_save')}</button>
      </div>
    </div>
  </>;
}
