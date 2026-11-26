import { clientDiagnostics } from '../data/clientDiagnostics';
import type { I18nKey } from '../i18n';
import { countCoverageQualityBands } from './coverageQuality';
import { deviceComputeProfilePromise } from './deviceComputeProfile';
import {
  deriveNegotiatedXrDepthCapability,
  isTagReadbackPerformanceSafe,
  probeVideoInputInventory,
  summarizeComputeCapabilities,
  type NegotiatedXrDepthCapability,
  type TagReadbackMode,
} from './xrCapabilityProfile';
import {
  DepthPointCloudAccumulator,
  type CpuDepthInformationLike,
  type DepthFusionFrameContext,
  type DepthSampleView,
  type LiveCoverageSnapshot,
  type ScanPointCloudSnapshot as DepthScanPointCloudSnapshot,
} from './depthPointCloud';
import {
  LiveCoveragePointRenderer,
  type CoverageRenderSnapshot,
  type CoverageRenderView,
  type CoverageRenderViewport,
} from './liveCoverageRenderer';
import type { ReferenceOverlay } from './fiducial/referenceOverlay';
import { LiveMeshRenderer, REFERENCE_MESH_STYLE } from './liveMeshRenderer';
import type { ReferenceMeshOverlay } from './fiducial/referenceOverlay';
import { LiveMeshClient } from './liveMeshClient';
import { LIVE_MESH_MAX_POINTS } from './liveMeshCore';
import { LIVE_MESH_MIN_INTERVAL_MS } from './liveMeshClient';
import { activeXrPerformancePressure } from './xrPerformanceProfiler';
import {
  ManualControlPointStore,
  ManualPlacementRejectedError,
  type ManualControlPoint,
  type ProjectPoint,
} from './manualControlPoints';
import { previewManualPlacementFromStableSurface } from './manualPlacementPreview';
import { optionalMonocularPrior } from './monocularPriorRuntime';
import { webXrLocalFloorToProject } from './scanViewportScene';
import { OperatorEdgeStore, type OperatorEdge } from './operatorEdges';
import {
  projectProjectPointToNormalized as projectProjectPointToScreen,
  type NormalizedProjection,
} from './projectionMath';
import {
  RawCameraGuidanceCapture,
  type XrCameraLike,
  type XrWebGlBindingConstructorLike,
  type XrWebGlBindingLike,
} from './rawCameraGuidance';
import { webGl2Context } from './webGlStateGuard';
import { tagCaptureSizeFor } from './tagCaptureSize';
import type { TagCameraFrame } from './fiducial/tagDetectorClient';
import { detectVisualLines } from './visualLineEvidence';
import {
  addVisualEvidenceFrame,
  getFreshRgbGuidance,
  getVisualEvidenceFrameCount,
  resetXrGuidanceRegistry,
  setLatestRgbGuidance,
} from './xrGuidanceRegistry';
import {
  resetLatestProjectionMatrices,
  setLatestProjectionMatrices,
} from './latestProjectionRegistry';
import {
  XrPerformanceProfiler,
  emptyXrPerformanceSnapshot,
  type XrPerformanceSnapshot,
} from './xrPerformanceProfiler';
import {
  WebXrFusionKeyframeSelector,
  type FusionMotionState,
} from './webxrFusionQuality';
import {
  depthSensingPauseAction,
  shouldPauseXrScan,
} from './webxrDepthPausePolicy';
import type { XrPlaneFrameLike } from './webxrPlaneGuidance';
import {
  captureLatestStructuralPlanePriors,
  getLatestStructuralPlanePriorCount,
  resetXrPlaneGuidanceRegistry,
} from './xrPlaneGuidanceRegistry';

export type XrRuntimePhase = 'idle'|'starting'|'running'|'stopping'|'ended'|'interrupted'|'error';

export interface ScanPointCloudSnapshot extends DepthScanPointCloudSnapshot {
  manualControlPoints: readonly ManualControlPoint[];
  operatorEdges: readonly OperatorEdge[];
}

export interface WebXrCapabilities {
  secureContext: boolean;
  /** Backward-compatible alias for rawXrRgbApi. */
  cameraApi: boolean;
  /** XRWebGLBinding exists; actual raw RGB is only proven by runtime capture. */
  rawXrRgbApi: boolean;
  tagReadbackMode: TagReadbackMode;
  webGl: boolean;
  xrApi: boolean;
  immersiveAr: boolean;
  wakeLockApi: boolean;
  videoInputEnumeration: boolean;
  videoInputsEnumerated: boolean;
  videoInputCount: number | null;
  videoInputLabelsExposed: boolean;
  /** Browser capability only: physical ToF/LiDAR/IR identity is never inferred. */
  hardwareDepthSensor: 'unknown';
  workerApi: boolean;
  sharedMemory: boolean;
  wasmThreads: boolean;
  webGpu: boolean;
}

export interface WebXrPoseSnapshot {
  position: readonly [number, number, number];
  orientation: readonly [number, number, number, number];
  views: number;
}

export interface WebXrRuntimeState {
  phase: XrRuntimePhase;
  referenceSpace: 'local-floor'|'local'|null;
  domOverlay: boolean;
  environmentBlendMode: string|null;
  frameCount: number;
  tracking: boolean;
  pose: WebXrPoseSnapshot|null;
  scanPaused: boolean;
  /** The tracking origin was reset inside this session (XRReferenceSpace 'reset'): evidence collection is paused so two coordinate frames are never mixed. */
  originReset: boolean;
  wakeLockActive: boolean;
  wakeLockError: string|null;
  depthAvailable: boolean;
  depthUsage: string|null;
  depthDataFormat: string|null;
  depthType: string|null;
  depthWidth: number|null;
  depthHeight: number|null;
  depthFrameCount: number;
  sampledDepthFrames: number;
  pointCount: number;
  retainedPointCount: number;
  pointCloudVersion: number;
  keyframeCount: number;
  polishFrameCount: number;
  motionRejectedFrames: number;
  motionState: FusionMotionState|'paused'|null;
  motionQuality: number;
  linearSpeedMetersPerSecond: number;
  angularSpeedRadiansPerSecond: number;
  linearAccelerationMetersPerSecond2: number;
  coverageGuideActive: boolean;
  coverageGuideRenderPoints: number;
  coverageVisibleCells: number;
  coverageReadyCells: number;
  coverageTotalCells: number;
  coverageTargetIndependentViews: number;
  coverageAverageConfidence: number;
  coverageWeakPoints: number;
  coverageImprovingPoints: number;
  coverageStablePoints: number;
  coverageGuideError: string|null;
  manualPointCount: number;
  manualEdgeCount: number;
  manualPointError: string|null;
  manualPointErrorKey: I18nKey|null;
  performance: XrPerformanceSnapshot;
  lightingEstimateAvailable: boolean;
  primaryLightIntensity: number|null;
  rgbGuidanceAvailable: boolean;
  rgbGuidanceWidth: number|null;
  rgbGuidanceHeight: number|null;
  visualEvidenceFrames: number;
  monocularPriorStatus: 'disabled'|'unavailable'|'loading'|'ready'|'error';
  depthError: string|null;
  error: string|null;
}

interface XrPointLike { x:number; y:number; z:number; w?:number; }
interface XrRigidTransformLike { position:XrPointLike; orientation:Required<XrPointLike>; matrix:ArrayLike<number>; }
interface XrViewLike extends DepthSampleView, CoverageRenderView { projectionMatrix:ArrayLike<number>; transform:XrRigidTransformLike; camera?:XrCameraLike|null; }
interface XrReferenceSpaceResetEventLike { transform?: unknown }
interface XrReferenceSpaceLike {
  addEventListener?(type: 'reset', listener: (event: XrReferenceSpaceResetEventLike) => void): void;
  removeEventListener?(type: 'reset', listener: (event: XrReferenceSpaceResetEventLike) => void): void;
}
interface XrLightProbeLike {}
interface XrLightEstimateLike { primaryLightIntensity:XrPointLike; }
interface XrViewerPoseLike { transform:XrRigidTransformLike; views:readonly XrViewLike[]; }
interface XrFrameLike extends XrPlaneFrameLike {
  session:XrSessionLike;
  getViewerPose(referenceSpace:XrReferenceSpaceLike):XrViewerPoseLike|null;
  getDepthInformation?(view:XrViewLike):CpuDepthInformationLike|null;
  getLightEstimate?(probe:XrLightProbeLike):XrLightEstimateLike|null;
}
type XrFrameCallback=(time:number,frame:XrFrameLike)=>void;
interface XrWebGlLayerLike {
  framebuffer:WebGLFramebuffer|null; framebufferWidth:number; framebufferHeight:number;
  getViewport(view:CoverageRenderView):CoverageRenderViewport;
}
interface XrSessionLike extends EventTarget {
  requestReferenceSpace(type:'local-floor'|'local'):Promise<XrReferenceSpaceLike>;
  requestAnimationFrame(callback:XrFrameCallback):number;
  cancelAnimationFrame?(handle:number):void;
  updateRenderState(state:{baseLayer:XrWebGlLayerLike}):void|Promise<void>;
  end():Promise<void>;
  requestLightProbe?():Promise<XrLightProbeLike>;
  pauseDepthSensing?():void;
  resumeDepthSensing?():void;
  domOverlayState?:{type:string}|null;
  environmentBlendMode?:string;
  visibilityState?:'visible'|'visible-blurred'|'hidden';
  depthUsage?:string; depthDataFormat?:string; depthType?:string|null; depthActive?:boolean|null;
}
interface XrSessionInitLike {
  optionalFeatures?:string[]; domOverlay?:{root:Element};
  depthSensing?:{
    usagePreference:string[]; dataFormatPreference:string[];
    depthTypeRequest?:string[]; matchDepthView?:boolean;
  };
}
interface XrSystemLike {
  isSessionSupported(mode:'immersive-ar'):Promise<boolean>;
  requestSession(mode:'immersive-ar',options?:XrSessionInitLike):Promise<XrSessionLike>;
}
type XrCompatibleGl=(WebGLRenderingContext|WebGL2RenderingContext)&{makeXRCompatible?:()=>Promise<void>};
type XrWebGlLayerConstructor=new(session:XrSessionLike,context:XrCompatibleGl)=>XrWebGlLayerLike;
type NavigatorWithXr=Navigator&{xr?:XrSystemLike};
type WindowWithXr=Window&{XRWebGLLayer?:XrWebGlLayerConstructor;XRWebGLBinding?:XrWebGlBindingConstructorLike};
interface WakeLockSentinelLike extends EventTarget { released:boolean; release():Promise<void>; }
interface WakeLockLike { request(type:'screen'):Promise<WakeLockSentinelLike>; }
type NavigatorWithWakeLock=Navigator&{wakeLock?:WakeLockLike};
type RuntimeListener=(state:WebXrRuntimeState)=>void;
type TagFrameListener=(frame:TagCameraFrame)=>void;
type DepthConfiguration = NegotiatedXrDepthCapability & { enabled:boolean; };

const DEPTH_SAMPLE_INTERVAL_MS=140;
const COVERAGE_GPU_UPLOAD_INTERVAL_MS=550;
const LIGHT_ESTIMATE_INTERVAL_MS=500;
const CLIENT_TELEMETRY_INTERVAL_MS=5000;
const RGB_CAPTURE_INTERVAL_MS=520;
const TAG_CAPTURE_INTERVAL_MS=330;
/* 1280x720, up from 960x540 (board 2026-11-17: a 32 mm tag was lost beyond ~0.6 m on a soft image). The tag detector needs >=12 px per edge,
   so the reach grows with the frame width: x1.33. The busy gate in XrRuntimePanel keeps a slower detection from queueing frames. */
export const TAG_CAPTURE_WIDTH=1280;
export const TAG_CAPTURE_HEIGHT=720;
const TAG_CAPTURE_READBACK_REQUIRED='High-resolution tag capture requires non-blocking WebGL2 readback.';
const VISUAL_EVIDENCE_INTERVAL_MS=760;
const PLANE_GUIDANCE_INTERVAL_MS=350;

const initialRuntimeState:WebXrRuntimeState={
  phase:'idle',referenceSpace:null,domOverlay:false,environmentBlendMode:null,frameCount:0,tracking:false,pose:null,
  scanPaused:false,originReset:false,wakeLockActive:false,wakeLockError:null,depthAvailable:false,depthUsage:null,depthDataFormat:null,
  depthType:null,depthWidth:null,depthHeight:null,depthFrameCount:0,sampledDepthFrames:0,pointCount:0,retainedPointCount:0,
  pointCloudVersion:0,keyframeCount:0,polishFrameCount:0,motionRejectedFrames:0,motionState:null,motionQuality:0,
  linearSpeedMetersPerSecond:0,angularSpeedRadiansPerSecond:0,linearAccelerationMetersPerSecond2:0,
  coverageGuideActive:false,coverageGuideRenderPoints:0,coverageVisibleCells:0,coverageReadyCells:0,coverageTotalCells:0,
  coverageTargetIndependentViews:3,coverageAverageConfidence:0,coverageWeakPoints:0,coverageImprovingPoints:0,coverageStablePoints:0,
  coverageGuideError:null,manualPointCount:0,manualEdgeCount:0,manualPointError:null,manualPointErrorKey:null,performance:emptyXrPerformanceSnapshot,
  lightingEstimateAvailable:false,primaryLightIntensity:null,rgbGuidanceAvailable:false,rgbGuidanceWidth:null,rgbGuidanceHeight:null,
  visualEvidenceFrames:0,monocularPriorStatus:'disabled',depthError:null,error:null,
};

function clamp01(value:number):number{return Math.max(0,Math.min(1,value));}
function createWebGlContext():XrCompatibleGl|null{
  const canvas=document.createElement('canvas');
  const attributes:WebGLContextAttributes={alpha:true,antialias:false,depth:true,stencil:false,premultipliedAlpha:true,preserveDrawingBuffer:false};
  const gl2=canvas.getContext('webgl2',attributes);if(gl2!==null)return gl2 as XrCompatibleGl;
  const gl=canvas.getContext('webgl',attributes);return gl===null?null:gl as XrCompatibleGl;
}
function releaseWebGlContext(context:XrCompatibleGl|null):void{context?.getExtension('WEBGL_lose_context')?.loseContext();}
function hasWebGl():boolean{const c=createWebGlContext();if(c===null)return false;releaseWebGlContext(c);return true;}
function probeTagReadbackMode():TagReadbackMode{
  let context:XrCompatibleGl|null=null;let capture:RawCameraGuidanceCapture|null=null;
  try{
    context=createWebGlContext();if(context===null)return'unavailable';
    capture=new RawCameraGuidanceCapture(context,1,1);
    return capture.tagReadbackMode;
  }catch{return'unavailable';}
  finally{try{capture?.dispose();}catch{/* capability probe is advisory */}try{releaseWebGlContext(context);}catch{/* capability probe is advisory */}}
}
function errorText(error:unknown):string{return error instanceof Error?error.message:String(error);}
function readDepthConfiguration(session:XrSessionLike):DepthConfiguration{
  try{const negotiated=deriveNegotiatedXrDepthCapability(session);return{...negotiated,enabled:negotiated.cpuDepth};}
  catch{return{enabled:false,active:false,usage:null,dataFormat:null,type:null,rawDepth:false,smoothDepth:false,cpuDepth:false,gpuDepth:false};}
}
function depthGeometry(depth:CpuDepthInformationLike,fallbackView:XrViewLike):DepthSampleView{
  if(depth.projectionMatrix!==undefined&&depth.projectionMatrix.length>=16&&depth.transform!==undefined&&depth.transform.matrix.length>=16)return{projectionMatrix:depth.projectionMatrix,transform:depth.transform};
  return fallbackView;
}
function projectCameraPosition(pose:XrViewerPoseLike):readonly[number,number,number]{const p=pose.transform.position;return webXrLocalFloorToProject(p.x,p.y,p.z);}

/** Copy snapshot and manual positions once into reusable views for the GPU upload path. */
export function composeCoverageRenderSnapshot(coverage:LiveCoverageSnapshot,manualReferencePositions:Float32Array,reusable?:{positions:Float32Array;confidence:Float32Array}):CoverageRenderSnapshot{
  const coverageCount=Math.min(coverage.pointCount,Math.floor(coverage.positions.length/3),coverage.confidence.length);
  const manualCount=Math.floor(manualReferencePositions.length/3),pointCount=coverageCount+manualCount;
  let positions:Float32Array;let confidence:Float32Array;
  if(reusable===undefined){positions=new Float32Array(pointCount*3);confidence=new Float32Array(pointCount);}
  else{
    if(reusable.positions.length<pointCount*3){let capacity=Math.max(16,reusable.positions.length);while(capacity<pointCount*3)capacity=Math.max(pointCount*3,capacity*2);reusable.positions=new Float32Array(capacity);}
    if(reusable.confidence.length<pointCount){let capacity=Math.max(16,reusable.confidence.length);while(capacity<pointCount)capacity=Math.max(pointCount,capacity*2);reusable.confidence=new Float32Array(capacity);}
    positions=reusable.positions;confidence=reusable.confidence;
  }
  if(coverageCount>0){positions.set(coverage.positions.subarray(0,coverageCount*3),0);confidence.set(coverage.confidence.subarray(0,coverageCount),0);}
  if(manualCount>0){positions.set(manualReferencePositions.subarray(0,manualCount*3),coverageCount*3);confidence.fill(2,coverageCount,pointCount);}
  return{positions:positions.subarray(0,pointCount*3),confidence:confidence.subarray(0,pointCount),pointCount,manualPointCount:manualCount};
}

export async function probeWebXrCapabilities():Promise<WebXrCapabilities>{
  const xr=(navigator as NavigatorWithXr).xr;const xrApi=xr!==undefined;let immersiveAr=false;
  if(xrApi&&window.isSecureContext){try{immersiveAr=await xr.isSessionSupported('immersive-ar');}catch{immersiveAr=false;}}
  const [computeProfile,videoInputs]=await Promise.all([
    deviceComputeProfilePromise,
    probeVideoInputInventory(typeof navigator==='undefined'?undefined:navigator.mediaDevices),
  ]);
  const compute=summarizeComputeCapabilities(computeProfile);
  const rawXrRgbApi=Boolean((window as WindowWithXr).XRWebGLBinding);
  const tagReadbackMode=probeTagReadbackMode();
  clientDiagnostics.info('XR_TAG_READBACK_CAPABILITY',{tagReadbackMode});void clientDiagnostics.flush();
  return{
    secureContext:window.isSecureContext===true,cameraApi:rawXrRgbApi,rawXrRgbApi,tagReadbackMode,webGl:hasWebGl(),xrApi,immersiveAr,
    wakeLockApi:(navigator as NavigatorWithWakeLock).wakeLock!==undefined,
    videoInputEnumeration:videoInputs.enumerationAvailable,videoInputsEnumerated:videoInputs.enumerated,
    videoInputCount:videoInputs.videoInputCount,videoInputLabelsExposed:videoInputs.labelsExposed,
    hardwareDepthSensor:videoInputs.hardwareDepthSensor,...compute,
  };
}

export type LiveMeshMode='auto'|'on'|'off';

export class WebXrRuntime{
  private state:WebXrRuntimeState=initialRuntimeState;
  private listeners=new Set<RuntimeListener>();
  private session:XrSessionLike|null=null;private referenceSpace:XrReferenceSpaceLike|null=null;private gl:XrCompatibleGl|null=null;
  private lifecycleGeneration=0;private disposed=false;private disposePromise:Promise<void>|null=null;private detachPromise:Promise<void>|null=null;private attached=false;private profilerActivationToken:number|null=null;
  private sessionEndListener:(()=>void)|null=null;private sessionVisibilityListener:(()=>void)|null=null;
  private readonly sessionEndRequests=new WeakMap<object,Promise<void>>();
  private rafHandle:number|null=null;
  private layer:XrWebGlLayerLike|null=null;private coverageRenderer:LiveCoveragePointRenderer|null=null;private referenceRenderer:LiveCoveragePointRenderer|null=null;private referenceOverlay:ReferenceOverlay|null=null;private referenceOverlayDirty=false;private liveMeshMode:LiveMeshMode='auto';private liveMeshRenderer:LiveMeshRenderer|null=null;private liveMeshClient:LiveMeshClient|null=null;private liveMeshPending:{trianglesXr:Float32Array;triangleCount:number}|null=null;private liveMeshOff=false;private liveMeshLoggedActive=false;private liveMeshModeLogged=false;private referenceMesh:ReferenceMeshOverlay|null=null;private referenceMeshDirty=false;private referenceMeshRenderer:LiveMeshRenderer|null=null;
  private cameraBinding:XrWebGlBindingLike|null=null;private cameraCapture:RawCameraGuidanceCapture|null=null;
  private tagCameraCapture:RawCameraGuidanceCapture|null=null;private tagCaptureEnabled=false;private tagGeometryLogged=false;private tagCaptureWidth=0;private tagCaptureHeight=0;private lastTagCaptureMs=Number.NEGATIVE_INFINITY;
  private tagFrameListeners=new Set<TagFrameListener>();
  private lightProbe:XrLightProbeLike|null=null;private wakeLock:WakeLockSentinelLike|null=null;
  private wakeLockReleaseListener:(()=>void)|null=null;private wakeLockRequest:{generation:number;promise:Promise<void>}|null=null;
  private readonly wakeLockReleases=new Set<Promise<void>>();
  private readonly frameFaultCounts=new Map<string,number>();private readonly loggedFrameFaults=new Set<string>();private frameFaultedGeneration:number|null=null;private fatalFrameTeardown=false;
  private contextCanvas:EventTarget|null=null;private contextLostListener:((event:Event)=>void)|null=null;private contextRestoredListener:(()=>void)|null=null;
  private finalSnapshot:ScanPointCloudSnapshot|null=null;
  private depthGateBlocked=false;private explicitStopRequested=false;private evidenceFrozen=false;private evidenceEpoch=0;private frozenVisualEvidenceFrames=0;private scanPaused=false;private originResetPaused=false;private referenceSpaceResetCleanup:(()=>void)|null=null;private frameCount=0;private lastUiUpdateMs=0;
  private lastDepthSampleMs=Number.NEGATIVE_INFINITY;private lastCoverageGpuUploadMs=Number.NEGATIVE_INFINITY;
  private lastLightEstimateMs=Number.NEGATIVE_INFINITY;private lastClientTelemetryMs=Number.NEGATIVE_INFINITY;
  private lastRgbCaptureMs=Number.NEGATIVE_INFINITY;private lastVisualEvidenceMs=Number.NEGATIVE_INFINITY;private lastPlaneGuidanceMs=Number.NEGATIVE_INFINITY;
  private depthFrameCount=0;private sampledDepthFrames=0;private pointCloudVersion=0;private depthWidth:number|null=null;private depthHeight:number|null=null;
  private depthFailureReported=false;private depthReadyLogged=false;private depthEnabled=false;private depthSensingPausedByApp=false;private motionRejectedFrames=0;
  private motionState:FusionMotionState|'paused'|null=null;private motionQuality=0;private linearSpeedMetersPerSecond=0;
  private angularSpeedRadiansPerSecond=0;private linearAccelerationMetersPerSecond2=0;
  private coverageGuideActive=false;private coverageGuideRenderPoints=0;private coverageVisibleCells=0;private coverageReadyCells=0;
  private coverageTotalCells=0;private coverageTargetIndependentViews=3;private coverageAverageConfidence=0;
  private coverageWeakPoints=0;private coverageImprovingPoints=0;private coverageStablePoints=0;private coverageGuideError:string|null=null;
  private lightingEstimateAvailable=false;private primaryLightIntensity:number|null=null;
  private rgbGuidanceAvailable=false;private rgbGuidanceWidth:number|null=null;private rgbGuidanceHeight:number|null=null;
  private readonly pointCloud=new DepthPointCloudAccumulator();private readonly keyframeSelector=new WebXrFusionKeyframeSelector();
  private readonly manualPoints=new ManualControlPointStore();private readonly operatorEdges=new OperatorEdgeStore();
  private readonly coverageCompositionBuffers={positions:new Float32Array(0),confidence:new Float32Array(0)};
  private readonly performanceProfiler=new XrPerformanceProfiler();private readonly latestProjectionMatrix=new Float32Array(16);
  private readonly latestWorldFromView=new Float32Array(16);private hasLatestProjection=false;

  attach():void{
    if(this.disposed||this.attached)return;
    this.attached=true;
    document.addEventListener('visibilitychange',this.handleDocumentVisibilityChange);
    this.profilerActivationToken=this.performanceProfiler.activate();
  }
  detach():Promise<void>{
    if(this.detachPromise!==null){
      if(!this.attached)return this.detachPromise;
      // A StrictMode replay can re-attach while the first cleanup is still
      // awaiting session or wake-lock completion. A later real unmount must
      // release that replay attachment too.
      this.attached=false;
      document.removeEventListener('visibilitychange',this.handleDocumentVisibilityChange);
      if(this.profilerActivationToken!==null)this.performanceProfiler.deactivate(this.profilerActivationToken);
      this.profilerActivationToken=null;
      this.lifecycleGeneration+=1;this.tagFrameListeners.clear();this.finalSnapshot=null;
      if(!this.disposed)this.state={...initialRuntimeState};
      const pending=this.detachPromise;
      let tracked:Promise<void>;
      tracked=Promise.all([pending,this.waitForWakeLockReleases()]).then(()=>undefined).finally(()=>{if(this.detachPromise===tracked)this.detachPromise=null;});
      this.detachPromise=tracked;
      return tracked;
    }
    if(!this.attached&&this.session===null&&this.gl===null)return Promise.resolve();
    if(this.attached){
      this.attached=false;
      document.removeEventListener('visibilitychange',this.handleDocumentVisibilityChange);
      if(this.profilerActivationToken!==null)this.performanceProfiler.deactivate(this.profilerActivationToken);
      this.profilerActivationToken=null;
    }
    this.lifecycleGeneration+=1;
    const session=this.session;
    this.tagFrameListeners.clear();this.finalSnapshot=null;
    this.cleanupRuntime();
    if(!this.disposed)this.state={...initialRuntimeState};
    const work=(async()=>{if(session!==null)await this.endObsoleteSession(session,true);await this.waitForWakeLockReleases();})();
    let tracked:Promise<void>;
    tracked=work.finally(()=>{if(this.detachPromise===tracked)this.detachPromise=null;});
    this.detachPromise=tracked;
    return tracked;
  }
  subscribe(listener:RuntimeListener):()=>void{if(this.disposed)return()=>undefined;this.listeners.add(listener);listener(this.state);return()=>this.listeners.delete(listener);}
  subscribeTagFrames(listener:TagFrameListener):()=>void{if(this.disposed)return()=>undefined;this.tagFrameListeners.add(listener);return()=>this.tagFrameListeners.delete(listener);}
  setTagCaptureEnabled(enabled:boolean):string|null{
    if(this.disposed)return enabled?'XR camera capture is unavailable for this session.':null;
    if(!enabled){this.tagCaptureEnabled=false;this.tagCameraCapture?.dispose();this.tagCameraCapture=null;this.lastTagCaptureMs=Number.NEGATIVE_INFINITY;return null;}
    if(this.state.phase!=='running'||this.evidenceFrozen||this.originResetPaused||this.gl===null||this.cameraBinding===null)return'XR camera capture is unavailable for this session.';
    if(webGl2Context(this.gl)===null){clientDiagnostics.warn('XR_TAG_CAPTURE_REFUSED',{tagReadbackMode:'synchronous',width:TAG_CAPTURE_WIDTH,height:TAG_CAPTURE_HEIGHT,reason:'webgl1'});void clientDiagnostics.flush();return TAG_CAPTURE_READBACK_REQUIRED;}
    try{
      this.tagCameraCapture??=new RawCameraGuidanceCapture(this.gl, TAG_CAPTURE_WIDTH, TAG_CAPTURE_HEIGHT);
      if(!isTagReadbackPerformanceSafe(this.tagCameraCapture.tagReadbackMode)){
        const tagReadbackMode=this.tagCameraCapture.tagReadbackMode;this.tagCameraCapture.dispose();this.tagCameraCapture=null;
        clientDiagnostics.warn('XR_TAG_CAPTURE_REFUSED',{tagReadbackMode,width:TAG_CAPTURE_WIDTH,height:TAG_CAPTURE_HEIGHT,reason:'nonblocking-readback-unavailable'});void clientDiagnostics.flush();return TAG_CAPTURE_READBACK_REQUIRED;
      }
      this.tagCaptureEnabled=true;this.tagGeometryLogged=false;this.tagCaptureWidth=TAG_CAPTURE_WIDTH;this.tagCaptureHeight=TAG_CAPTURE_HEIGHT;this.lastTagCaptureMs=Number.NEGATIVE_INFINITY;
      clientDiagnostics.info('XR_TAG_CAPTURE_ENABLED',{tagReadbackMode:this.tagCameraCapture.tagReadbackMode,width:TAG_CAPTURE_WIDTH,height:TAG_CAPTURE_HEIGHT});void clientDiagnostics.flush();
      return null;
    }
    catch(error){this.tagCameraCapture?.dispose();this.tagCameraCapture=null;clientDiagnostics.warn('XR_TAG_CAPTURE_REFUSED',{tagReadbackMode:'unavailable',width:TAG_CAPTURE_WIDTH,height:TAG_CAPTURE_HEIGHT,reason:errorText(error)});void clientDiagnostics.flush();return`High-resolution tag capture unavailable: ${errorText(error)}`;}
  }
  getState():WebXrRuntimeState{return this.state;}
  getPointCloudSnapshot():ScanPointCloudSnapshot{return this.finalSnapshot??this.createPointCloudSnapshot();}
  getLifecycleGeneration():number{return this.lifecycleGeneration;}
  getLastManualPointId():number|null{return this.manualPoints.lastId;}
  getStructuralWorkerSnapshot(){return this.pointCloud.structuralWorkerSnapshot();}
  getStructuralGuideSnapshot(){return this.pointCloud.structuralGuideSnapshot();}
  getManualControlPoints():readonly ManualControlPoint[]{return this.manualPoints.snapshot();}
  getOperatorEdges():readonly OperatorEdge[]{return this.operatorEdges.snapshot();}
  projectProjectPointToNormalized(point:ProjectPoint):NormalizedProjection|null{return this.hasLatestProjection?projectProjectPointToScreen(point,this.latestProjectionMatrix,this.latestWorldFromView):null;}

  addSuggestedManualPoint(position:ProjectPoint):{point:ManualControlPoint;created:boolean;snapped:boolean}|null{
    if(this.session===null||this.state.phase!=='running'||this.evidenceFrozen||this.originResetPaused)return null;
    try{
      const stored=this.manualPoints.addOrSnap(position);this.lastCoverageGpuUploadMs=Number.NEGATIVE_INFINITY;this.publish({manualPointCount:this.manualPoints.size,manualPointError:null,manualPointErrorKey:null});return stored;
    }catch(error){
      if(error instanceof ManualPlacementRejectedError&&error.i18nKey!==undefined)this.publish({manualPointError:error.message,manualPointErrorKey:error.i18nKey});
      throw error;
    }
  }
  addOperatorEdge(start:ProjectPoint,end:ProjectPoint,confidence=1):OperatorEdge|null{
    if(this.session===null||this.state.phase!=='running'||this.evidenceFrozen||this.originResetPaused)return null;const stored=this.operatorEdges.add(start,end,confidence);
    if(stored!==null)this.publish({manualEdgeCount:this.operatorEdges.size,manualPointError:null,manualPointErrorKey:null});
    else if(this.operatorEdges.lastAddRejectionReason==='capacity')this.publish({manualPointError:null,manualPointErrorKey:'xr.manual.edge_capacity_reached'});
    return stored;
  }

  /** Robust manual placement only. No screen coordinate is ever converted from one raw depth texel. */
  queueManualPointAtNormalized(u:number,v:number,pointId:number|null=null):void{
    if(this.session===null||this.state.phase!=='running'||this.evidenceFrozen||this.originResetPaused){this.publish({manualPointError:'Manual point can only be placed during an active AR scan.',manualPointErrorKey:null});return;}
    try{
      const preview=previewManualPlacementFromStableSurface(clamp01(u),clamp01(v),this.getStructuralGuideSnapshot(),(point)=>this.projectProjectPointToNormalized(point));
      const resolution=preview.resolution;if(!resolution.accepted||resolution.position===null){this.publish({manualPointError:resolution.message,manualPointErrorKey:null});return;}
      const stored=pointId===null?this.manualPoints.addOrSnap(resolution.position):this.manualPoints.update(pointId,resolution.position);
      if(stored===null){this.publish({manualPointError:'The selected manual point no longer exists.',manualPointErrorKey:null});return;}
      this.lastCoverageGpuUploadMs=Number.NEGATIVE_INFINITY;this.publish({manualPointCount:this.manualPoints.size,manualPointError:null,manualPointErrorKey:null});
      clientDiagnostics.info(pointId===null?'XR_MANUAL_CERTIFIED_POINT_ADDED':'XR_MANUAL_CERTIFIED_POINT_MOVED',{id:stored.id,mode:resolution.mode,confidence:Number(resolution.confidence.toFixed(3)),x:Number(stored.position[0].toFixed(3)),y:Number(stored.position[1].toFixed(3)),z:Number(stored.position[2].toFixed(3))});
    }catch(error){this.publish({manualPointError:errorText(error),manualPointErrorKey:error instanceof ManualPlacementRejectedError?error.i18nKey??null:null});clientDiagnostics.warn('XR_MANUAL_CERTIFIED_POINT_FAILED',error);}
  }
  undoLastManualPoint():void{if(this.evidenceFrozen)return;const removed=this.manualPoints.removeLast();if(removed===null)return;this.lastCoverageGpuUploadMs=Number.NEGATIVE_INFINITY;this.publish({manualPointCount:this.manualPoints.size,manualPointError:null,manualPointErrorKey:null});clientDiagnostics.info('XR_MANUAL_POINT_UNDO',{id:removed.id,remaining:this.manualPoints.size});void clientDiagnostics.flush();}

  async start():Promise<void>{
    if(this.disposed||!this.attached||this.detachPromise!==null||this.fatalFrameTeardown||this.session!==null||this.state.phase==='starting')return;clientDiagnostics.info('XR_START_REQUEST');void clientDiagnostics.flush();
    const generation=++this.lifecycleGeneration;
    const ownsAttempt=():boolean=>!this.disposed&&this.lifecycleGeneration===generation;
    const xr=(navigator as NavigatorWithXr).xr;const Layer=(window as WindowWithXr).XRWebGLLayer;
    if(!window.isSecureContext){this.publish({phase:'error',error:'Trusted HTTPS is required for WebXR.'});return;}
    if(xr===undefined){this.publish({phase:'error',error:'WebXR is not available in this browser.'});return;}
    if(Layer===undefined){this.publish({phase:'error',error:'XRWebGLLayer is unavailable.'});return;}
    this.finalSnapshot=null;this.resetScanState();this.publish({...initialRuntimeState,phase:'starting',monocularPriorStatus:optionalMonocularPrior.status});if(!ownsAttempt())return;
    let session:XrSessionLike|null=null;
    try{
      // luminance-alpha remains first because every depth-capable UA MUST support
      // it; unsigned-short is the cleaner current 16-bit representation when
      // available, and float32 remains the final compatibility fallback.
      const depthFormats=['luminance-alpha','unsigned-short','float32'];
      session=await xr.requestSession('immersive-ar',{
        optionalFeatures:['local-floor','dom-overlay','depth-sensing','light-estimation','camera-access','plane-detection'],domOverlay:{root:document.body},
        depthSensing:{usagePreference:['cpu-optimized','gpu-optimized'],dataFormatPreference:depthFormats,depthTypeRequest:['raw','smooth'],matchDepthView:true},
      });
      if(!ownsAttempt()){await this.endObsoleteSession(session);return;}
      this.attachSession(session,generation);
      const gl=createWebGlContext();if(gl===null)throw new Error('Unable to create WebGL for WebXR.');this.gl=gl;this.attachWebGlContextHandlers(gl,session,generation);
      await gl.makeXRCompatible?.();if(!ownsAttempt())return;
      const layer=new Layer(session,gl);this.layer=layer;await session.updateRenderState({baseLayer:layer});if(!ownsAttempt())return;
      let referenceSpaceType:'local-floor'|'local'='local-floor';let referenceSpace:XrReferenceSpaceLike;
      try{referenceSpace=await session.requestReferenceSpace('local-floor');if(!ownsAttempt())return;}
      catch(error){if(!ownsAttempt())return;referenceSpaceType='local';referenceSpace=await session.requestReferenceSpace('local');if(!ownsAttempt())return;}
      this.referenceSpace=referenceSpace;this.watchReferenceSpaceReset(referenceSpace,session,generation);
      try{this.coverageRenderer=new LiveCoveragePointRenderer(gl);}catch(error){this.coverageRenderer=null;this.coverageGuideError=`Coverage renderer unavailable: ${errorText(error)}`;}
      const Binding=(window as WindowWithXr).XRWebGLBinding;
      if(Binding!==undefined){try{this.cameraBinding=new Binding(session,gl);this.cameraCapture=new RawCameraGuidanceCapture(gl);}catch(error){this.cameraBinding=null;this.cameraCapture=null;clientDiagnostics.warn('XR_RAW_CAMERA_UNAVAILABLE',error);}}
      if(typeof session.requestLightProbe==='function'){try{const lightProbe=await session.requestLightProbe();if(!ownsAttempt())return;this.lightProbe=lightProbe;}catch{if(!ownsAttempt())return;this.lightProbe=null;}}
      if(!ownsAttempt())return;
      document.documentElement.classList.add('xr-session-active');
      if(session.environmentBlendMode==='alpha-blend'||session.environmentBlendMode==='additive')document.documentElement.classList.add('xr-passthrough-active');
      const depthConfiguration=readDepthConfiguration(session);this.depthEnabled=depthConfiguration.enabled;void this.acquireWakeLock(generation);
      this.publish({phase:'running',referenceSpace:referenceSpaceType,domOverlay:session.domOverlayState!=null,environmentBlendMode:session.environmentBlendMode??null,
        depthAvailable:depthConfiguration.enabled,depthUsage:depthConfiguration.usage,depthDataFormat:depthConfiguration.dataFormat,depthType:depthConfiguration.type,
        coverageGuideError:this.coverageGuideError,wakeLockActive:this.wakeLock!==null&&!this.wakeLock.released,monocularPriorStatus:optionalMonocularPrior.status,
        depthError:depthConfiguration.enabled?null:depthConfiguration.gpuDepth?'WebXR negotiated GPU-optimized depth, but metric surfel fusion currently requires CPU depth access.':'WebXR tracking is running, but CPU depth was not enabled.',error:null});
      clientDiagnostics.info('XR_SESSION_RUNNING',{referenceSpace:referenceSpaceType,domOverlay:session.domOverlayState!=null,environmentBlendMode:session.environmentBlendMode??null,depthEnabled:depthConfiguration.enabled,depthActive:depthConfiguration.active,depthUsage:depthConfiguration.usage,depthFormat:depthConfiguration.dataFormat,depthType:depthConfiguration.type,rawDepth:depthConfiguration.rawDepth,smoothDepth:depthConfiguration.smoothDepth,cpuDepth:depthConfiguration.cpuDepth,gpuDepth:depthConfiguration.gpuDepth,cameraAccessRequested:true,rawXrRgbApi:Binding!==undefined,planeDetectionRequested:true,rgbReadback:this.cameraCapture?.usesNonBlockingReadback?'pbo-fence':'sync-or-unavailable',monocularPriorStatus:optionalMonocularPrior.status});
      if(!ownsAttempt())return;void clientDiagnostics.flush();this.rafHandle=session.requestAnimationFrame(this.handleFrame);
    }catch(error){
      if(!ownsAttempt())return;
      const failedSession=session;this.cleanupRuntime();if(failedSession!==null)await this.endObsoleteSession(failedSession,true);await this.waitForWakeLockReleases();
      const message=errorText(error);clientDiagnostics.error('XR_START_FAILED',message);void clientDiagnostics.flush();
      if(this.disposed)return;
      this.publish({phase:'error',referenceSpace:null,domOverlay:false,environmentBlendMode:null,tracking:false,pose:null,scanPaused:false,depthAvailable:false,monocularPriorStatus:optionalMonocularPrior.status,error:message});
    }
  }

  async stop():Promise<void>{
    const session=this.session;const generation=this.lifecycleGeneration;if(session===null||this.state.phase==='stopping'||this.disposed)return;
    this.evidenceFrozen=true;this.explicitStopRequested=true;this.finalSnapshot??=this.createPointCloudSnapshot();this.frozenVisualEvidenceFrames=getVisualEvidenceFrameCount();this.tagCaptureEnabled=false;
    const snapshot=this.finalSnapshot;
    clientDiagnostics.info('XR_FINISH_REQUEST',{evidenceEpoch:this.evidenceEpoch,depthFrames:snapshot.depthFrameCount,integratedFrames:snapshot.sampledDepthFrames,stablePoints:snapshot.pointCount,retainedPoints:snapshot.rawPointCount,keyframes:snapshot.integrationKeyframes,polishFrames:snapshot.polishFrames,manualPoints:snapshot.manualControlPoints.length,operatorEdges:snapshot.operatorEdges.length,visualEvidenceFrames:this.frozenVisualEvidenceFrames});void clientDiagnostics.flush();this.publish({phase:'stopping',error:null});
    try{await this.requestSessionEnd(session);if(this.session===session&&this.lifecycleGeneration===generation)this.handleSessionEnd(session,generation);await this.waitForWakeLockReleases();}
    catch(error){if(this.session!==session||this.lifecycleGeneration!==generation||this.disposed)return;this.explicitStopRequested=false;this.publish({phase:'running',error:errorText(error)});}
  }

  dispose():Promise<void>{
    if(this.disposePromise!==null)return this.disposePromise;
    this.disposed=true;this.lifecycleGeneration+=1;
    this.disposePromise=(async()=>{await this.detach();this.listeners.clear();})();
    return this.disposePromise;
  }
  private publish(patch:Partial<WebXrRuntimeState>):void{this.state={...this.state,...patch};for(const listener of this.listeners)listener(this.state);}

  private recordFrameFault(faultClass:string,error:unknown):number{
    const count=(this.frameFaultCounts.get(faultClass)??0)+1;this.frameFaultCounts.set(faultClass,count);
    if(!this.loggedFrameFaults.has(faultClass)){
      this.loggedFrameFaults.add(faultClass);
      clientDiagnostics.warn('XR_FRAME_FAULT',{faultClass,generation:this.lifecycleGeneration,count,message:errorText(error)});
      void clientDiagnostics.flush();
    }
    return count;
  }

  private disableCoverageGuide(error:unknown):void{
    const faultClass='coverage-render';this.recordFrameFault(faultClass,error);
    try{this.coverageRenderer?.dispose();}catch{/* a failed optional renderer is already unusable */}
    this.coverageRenderer=null;this.coverageGuideActive=false;this.coverageGuideRenderPoints=0;
    this.coverageGuideError=`Coverage guide disabled: ${errorText(error)}`;
    this.publish({coverageGuideActive:false,coverageGuideRenderPoints:0,coverageGuideError:this.coverageGuideError});
  }

  private disableCameraGuidance(kind:'rgb'|'tag',error:unknown,faultClass:string):void{
    this.recordFrameFault(faultClass,error);
    const capture=kind==='rgb'?this.cameraCapture:this.tagCameraCapture;
    if(kind==='rgb'){
      this.cameraCapture=null;setLatestRgbGuidance(null);this.rgbGuidanceAvailable=false;this.rgbGuidanceWidth=null;this.rgbGuidanceHeight=null;
    }else{this.tagCameraCapture=null;this.tagCaptureEnabled=false;this.lastTagCaptureMs=Number.NEGATIVE_INFINITY;}
    try{capture?.dispose();}catch{/* optional camera resources may already be invalid */}
    this.coverageGuideError=`${kind==='rgb'?'RGB':'Tag'} camera guidance disabled: ${errorText(error)}`;
    this.publish({coverageGuideError:this.coverageGuideError,rgbGuidanceAvailable:this.rgbGuidanceAvailable,rgbGuidanceWidth:this.rgbGuidanceWidth,rgbGuidanceHeight:this.rgbGuidanceHeight});
  }

  private handleFatalFrameFault(session:XrSessionLike,generation:number,error:unknown,faultClass:string):void{
    if(this.disposed||this.session!==session||this.lifecycleGeneration!==generation||this.frameFaultedGeneration===generation)return;
    this.frameFaultedGeneration=generation;this.recordFrameFault(faultClass,error);this.fatalFrameTeardown=true;
    const message=`XR frame failed (${faultClass}): ${errorText(error)}`;
    try{this.publish({phase:'stopping',tracking:false,pose:null,scanPaused:false,error:null});}catch{/* continue deterministic teardown if a subscriber fails */}
    this.cleanupRuntime();
    const teardownGeneration=this.lifecycleGeneration;
    void (async()=>{
      try{await this.endObsoleteSession(session,true);await this.waitForWakeLockReleases();}
      finally{
        this.fatalFrameTeardown=false;
        if(!this.disposed&&this.lifecycleGeneration===teardownGeneration){
          try{this.publish({phase:'error',referenceSpace:null,domOverlay:false,environmentBlendMode:null,tracking:false,pose:null,scanPaused:false,wakeLockActive:false,depthAvailable:false,error:message});}
          catch{/* terminal state is already stored before listeners are called */}
        }
      }
    })();
  }

  private attachWebGlContextHandlers(gl:XrCompatibleGl,session:XrSessionLike,generation:number):void{
    const canvas=gl.canvas as unknown as EventTarget|null;
    if(canvas===null||typeof canvas.addEventListener!=='function')return;
    const onLost=(event:Event):void=>{
      // A lost XR context invalidates the renderer and readback resources, so this session is torn down as fatal.
      event.preventDefault();
      if(this.gl!==gl||this.session!==session||this.lifecycleGeneration!==generation)return;
      this.handleFatalFrameFault(session,generation,new Error('WebGL context was lost.'),'webgl-context-lost');
    };
    const onRestored=():void=>{
      // Context restoration after fatal teardown is intentionally ignored. The
      // XR session and its GL resources have already been released.
      if(this.gl!==gl||this.session!==session||this.lifecycleGeneration!==generation)return;
    };
    this.contextCanvas=canvas;this.contextLostListener=onLost;this.contextRestoredListener=onRestored;
    canvas.addEventListener('webglcontextlost',onLost);canvas.addEventListener('webglcontextrestored',onRestored);
  }

  private detachWebGlContextHandlers():void{
    if(this.contextCanvas!==null){
      try{if(this.contextLostListener!==null)this.contextCanvas.removeEventListener('webglcontextlost',this.contextLostListener);}catch{/* continue teardown */}
      try{if(this.contextRestoredListener!==null)this.contextCanvas.removeEventListener('webglcontextrestored',this.contextRestoredListener);}catch{/* continue teardown */}
    }
    this.contextCanvas=null;this.contextLostListener=null;this.contextRestoredListener=null;
  }

  private resetScanState():void{
    this.finalSnapshot=null;this.evidenceEpoch+=1;this.evidenceFrozen=false;this.frozenVisualEvidenceFrames=0;this.frameFaultCounts.clear();this.loggedFrameFaults.clear();this.frameFaultedGeneration=null;this.pointCloud.reset();this.keyframeSelector.reset();this.manualPoints.reset();this.operatorEdges.reset();this.performanceProfiler.reset();resetXrGuidanceRegistry();resetXrPlaneGuidanceRegistry();resetLatestProjectionMatrices();
    this.hasLatestProjection=false;this.explicitStopRequested=false;this.scanPaused=false;this.originResetPaused=false;this.depthSensingPausedByApp=false;this.frameCount=0;this.lastUiUpdateMs=0;this.lastDepthSampleMs=Number.NEGATIVE_INFINITY;
    this.lastCoverageGpuUploadMs=Number.NEGATIVE_INFINITY;this.lastLightEstimateMs=Number.NEGATIVE_INFINITY;this.lastClientTelemetryMs=Number.NEGATIVE_INFINITY;
    this.lastRgbCaptureMs=Number.NEGATIVE_INFINITY;this.lastVisualEvidenceMs=Number.NEGATIVE_INFINITY;this.lastPlaneGuidanceMs=Number.NEGATIVE_INFINITY;this.depthFrameCount=0;this.sampledDepthFrames=0;this.pointCloudVersion=0;
    this.depthWidth=null;this.depthHeight=null;this.depthFailureReported=false;this.depthReadyLogged=false;this.depthEnabled=false;this.motionRejectedFrames=0;this.motionState=null;
    this.motionQuality=0;this.linearSpeedMetersPerSecond=0;this.angularSpeedRadiansPerSecond=0;this.linearAccelerationMetersPerSecond2=0;
    this.coverageGuideActive=false;this.coverageGuideRenderPoints=0;this.coverageVisibleCells=0;this.coverageReadyCells=0;this.coverageTotalCells=0;
    this.coverageTargetIndependentViews=3;this.coverageAverageConfidence=0;this.coverageWeakPoints=0;this.coverageImprovingPoints=0;this.coverageStablePoints=0;this.coverageGuideError=null;
    this.lightProbe=null;this.lightingEstimateAvailable=false;this.primaryLightIntensity=null;this.rgbGuidanceAvailable=false;this.rgbGuidanceWidth=null;this.rgbGuidanceHeight=null;
  }

  private cleanupRuntime():void{
    this.lifecycleGeneration+=1;this.detachSessionListeners(this.session);this.unwatchReferenceSpaceReset();this.originResetPaused=false;
    if(this.session!==null&&this.rafHandle!==null){try{this.session.cancelAnimationFrame?.(this.rafHandle);}catch{/* session ended */}}this.rafHandle=null;
    this.detachWebGlContextHandlers();try{document.documentElement.classList.remove('xr-session-active','xr-passthrough-active');}catch{/* document is already tearing down */}
    try{this.coverageRenderer?.dispose();}catch{/* continue releasing the remaining XR resources */}this.coverageRenderer=null;
    try{this.referenceRenderer?.dispose();}catch{/* continue releasing the remaining XR resources */}this.referenceRenderer=null;this.referenceOverlay=null;this.referenceOverlayDirty=false;
    this.disposeLiveMesh();this.liveMeshOff=false;
    try{this.referenceMeshRenderer?.dispose();}catch{/* continue releasing the remaining XR resources */}this.referenceMeshRenderer=null;this.referenceMesh=null;this.referenceMeshDirty=false;
    try{this.cameraCapture?.dispose();}catch{/* continue releasing the remaining XR resources */}this.cameraCapture=null;
    try{this.tagCameraCapture?.dispose();}catch{/* continue releasing the remaining XR resources */}this.tagCameraCapture=null;
    this.tagCaptureEnabled=false;this.lastTagCaptureMs=Number.NEGATIVE_INFINITY;this.cameraBinding=null;
    setLatestRgbGuidance(null);resetXrPlaneGuidanceRegistry();resetLatestProjectionMatrices();this.lightProbe=null;void this.releaseWakeLock();
    try{releaseWebGlContext(this.gl);}catch{/* context loss can make WEBGL_lose_context unavailable */}
    this.session=null;this.referenceSpace=null;this.gl=null;this.layer=null;
    this.scanPaused=false;this.depthEnabled=false;this.depthSensingPausedByApp=false;this.coverageGuideActive=false;this.coverageGuideRenderPoints=0;this.rgbGuidanceAvailable=false;
  }

  /*
   * A reset moves the tracking origin: points, tags and voxels collected before it and the ones after it use different coordinate frames (issue #36).
   * The event transform is not used to re-base anything: the scan fails closed instead. Depth, tag, visual and RGB evidence stop, the operator is told,
   * and Finish keeps what was scanned before the reset. A reset of a stale session or reference space is ignored.
   */
  private watchReferenceSpaceReset(referenceSpace:XrReferenceSpaceLike,session:XrSessionLike,generation:number):void{
    this.unwatchReferenceSpaceReset();
    if(typeof referenceSpace.addEventListener!=='function')return;
    const onReset=(event:XrReferenceSpaceResetEventLike):void=>{
      if(this.session!==session||this.referenceSpace!==referenceSpace||this.lifecycleGeneration!==generation||this.originResetPaused)return;
      this.originResetPaused=true;
      clientDiagnostics.warn('XR_REFERENCE_SPACE_RESET',{transformAvailable:event?.transform!==undefined&&event.transform!==null,frameCount:this.frameCount,depthFrames:this.depthFrameCount,evidenceEpoch:this.evidenceEpoch,action:'evidence_paused'});
      void clientDiagnostics.flush();
      this.publish({originReset:true,scanPaused:true,motionState:'paused'});
    };
    referenceSpace.addEventListener('reset',onReset);
    this.referenceSpaceResetCleanup=():void=>{try{referenceSpace.removeEventListener?.('reset',onReset);}catch{/* the space is already gone */}};
  }

  private unwatchReferenceSpaceReset():void{const cleanup=this.referenceSpaceResetCleanup;this.referenceSpaceResetCleanup=null;cleanup?.();}

  private attachSession(session:XrSessionLike,generation:number):void{
    this.session=session;const onEnd=():void=>this.handleSessionEnd(session,generation);const onVisibility=():void=>this.handleSessionVisibilityChange(session,generation);
    this.sessionEndListener=onEnd;this.sessionVisibilityListener=onVisibility;session.addEventListener('end',onEnd);session.addEventListener('visibilitychange',onVisibility);
  }

  private detachSessionListeners(session:XrSessionLike|null):void{
    if(session!==null){try{if(this.sessionEndListener!==null)session.removeEventListener('end',this.sessionEndListener);}catch{/* continue teardown */}try{if(this.sessionVisibilityListener!==null)session.removeEventListener('visibilitychange',this.sessionVisibilityListener);}catch{/* continue teardown */}}
    this.sessionEndListener=null;this.sessionVisibilityListener=null;
  }

  private async endObsoleteSession(session:XrSessionLike,retryOnce=false):Promise<void>{
    try{await this.requestSessionEnd(session);}catch{
      if(retryOnce){try{await this.requestSessionEnd(session);}catch{/* best-effort cleanup of an unowned browser session */}}
    }
  }

  private requestSessionEnd(session:XrSessionLike):Promise<void>{
    const existing=this.sessionEndRequests.get(session);if(existing!==undefined)return existing;
    let request:Promise<void>;
    try{request=Promise.resolve(session.end());}catch(error){return Promise.reject(error);}
    const tracked=request.catch((error:unknown)=>{if(this.sessionEndRequests.get(session)===tracked)this.sessionEndRequests.delete(session);throw error;});
    this.sessionEndRequests.set(session,tracked);return tracked;
  }

  private createPointCloudSnapshot():ScanPointCloudSnapshot{
    return{...this.pointCloud.snapshot(this.depthFrameCount,this.sampledDepthFrames),manualControlPoints:this.manualPoints.snapshot(),operatorEdges:this.operatorEdges.snapshot()};
  }

  /** Must only be called from the active XR animation-frame callback. */
  private syncDepthSensingPauseInsideFrame():void{
    const session=this.session;if(session===null)return;
    const action=depthSensingPauseAction(this.depthEnabled,this.scanPaused,this.depthSensingPausedByApp);if(action===null)return;
    try{
      if(action==='pause'){
        if(typeof session.pauseDepthSensing!=='function')return;
        session.pauseDepthSensing();this.depthSensingPausedByApp=true;clientDiagnostics.info('XR_DEPTH_SENSING_PAUSED');
      }else{
        if(typeof session.resumeDepthSensing!=='function')return;
        session.resumeDepthSensing();this.depthSensingPausedByApp=false;clientDiagnostics.info('XR_DEPTH_SENSING_RESUMED');
      }
    }catch(error){this.recordFrameFault('depth-pause-control',error);}
  }

  private acquireWakeLock(generation:number):Promise<void>{
    const ownsAttempt=():boolean=>!this.disposed&&this.lifecycleGeneration===generation&&this.session!==null;
    if(!ownsAttempt()||document.visibilityState!=='visible'||(this.wakeLock!==null&&!this.wakeLock.released))return Promise.resolve();
    if(this.wakeLockRequest?.generation===generation)return this.wakeLockRequest.promise;
    const api=(navigator as NavigatorWithWakeLock).wakeLock;if(api===undefined){this.publish({wakeLockActive:false,wakeLockError:'Screen Wake Lock API unavailable.'});return Promise.resolve();}
    const request={generation,promise:Promise.resolve()};this.wakeLockRequest=request;
    request.promise=(async()=>{
      try{
        const sentinel=await api.request('screen');
        if(!ownsAttempt()||document.visibilityState!=='visible'){if(!sentinel.released){try{await sentinel.release();}catch{/*obsolete wake lock*/}}return;}
        this.wakeLock=sentinel;const onRelease=():void=>{if(this.wakeLock!==sentinel)return;this.wakeLock=null;this.wakeLockReleaseListener=null;this.publish({wakeLockActive:false});};
        this.wakeLockReleaseListener=onRelease;sentinel.addEventListener('release',onRelease);this.publish({wakeLockActive:true,wakeLockError:null});
      }catch(error){if(ownsAttempt())this.publish({wakeLockActive:false,wakeLockError:errorText(error)});}
      finally{if(this.wakeLockRequest===request)this.wakeLockRequest=null;}
    })();
    return request.promise;
  }
  private releaseWakeLock():Promise<void>{
    const sentinel=this.wakeLock;this.wakeLock=null;if(sentinel===null)return Promise.resolve();
    if(this.wakeLockReleaseListener!==null)sentinel.removeEventListener('release',this.wakeLockReleaseListener);this.wakeLockReleaseListener=null;
    if(sentinel.released)return Promise.resolve();
    const releasing=(async()=>{try{await sentinel.release();}catch{/*released*/}})();this.wakeLockReleases.add(releasing);
    void releasing.finally(()=>this.wakeLockReleases.delete(releasing));return releasing;
  }
  private async waitForWakeLockReleases():Promise<void>{while(this.wakeLockReleases.size>0)await Promise.all([...this.wakeLockReleases]);}
  private readonly handleDocumentVisibilityChange=():void=>{
    const hidden=document.visibilityState!=='visible';if(this.session!==null){this.scanPaused=shouldPauseXrScan(document.visibilityState,this.session.visibilityState);this.publish({scanPaused:this.scanPaused,motionState:this.scanPaused?'paused':this.motionState,wakeLockActive:hidden?false:this.state.wakeLockActive});if(hidden)void this.releaseWakeLock();else void this.acquireWakeLock(this.lifecycleGeneration);}
  };
  private handleSessionVisibilityChange(session:XrSessionLike,generation:number):void{
    if(this.session!==session||this.lifecycleGeneration!==generation)return;this.scanPaused=shouldPauseXrScan(document.visibilityState,session.visibilityState);this.publish({scanPaused:this.scanPaused,motionState:this.scanPaused?'paused':this.motionState});
    if(this.scanPaused)void this.releaseWakeLock();else void this.acquireWakeLock(generation);
  }

  private handleSessionEnd(session:XrSessionLike,generation:number):void{
    if(this.session!==session||this.lifecycleGeneration!==generation)return;
    const normalFinish=this.explicitStopRequested;const snapshot=normalFinish?(this.finalSnapshot??this.createPointCloudSnapshot()):null;this.finalSnapshot=snapshot;const performance=this.performanceProfiler.snapshot();
    const visualEvidenceFrames=normalFinish?this.frozenVisualEvidenceFrames:getVisualEvidenceFrameCount();
    const pointCount=snapshot?.pointCount??this.pointCloud.promotedCount;const retainedPointCount=snapshot?.rawPointCount??this.pointCloud.pointCount;
    const keyframeCount=snapshot?.integrationKeyframes??this.pointCloud.keyframeCount;const polishFrameCount=snapshot?.polishFrames??this.pointCloud.polishFrameCount;
    this.cleanupRuntime();this.explicitStopRequested=false;
    this.publish({phase:normalFinish?'ended':'interrupted',referenceSpace:null,domOverlay:false,environmentBlendMode:null,tracking:false,pose:null,scanPaused:false,wakeLockActive:false,
      depthFrameCount:this.depthFrameCount,sampledDepthFrames:this.sampledDepthFrames,pointCount,retainedPointCount,keyframeCount,
      polishFrameCount,pointCloudVersion:this.pointCloudVersion,coverageGuideActive:false,coverageGuideRenderPoints:0,coverageVisibleCells:this.coverageVisibleCells,
      coverageReadyCells:this.coverageReadyCells,coverageTotalCells:this.coverageTotalCells,coverageTargetIndependentViews:this.coverageTargetIndependentViews,coverageAverageConfidence:this.coverageAverageConfidence,
      coverageWeakPoints:this.coverageWeakPoints,coverageImprovingPoints:this.coverageImprovingPoints,coverageStablePoints:this.coverageStablePoints,manualPointCount:this.manualPoints.size,manualEdgeCount:this.operatorEdges.size,
      performance,lightingEstimateAvailable:this.lightingEstimateAvailable,primaryLightIntensity:this.primaryLightIntensity,rgbGuidanceAvailable:this.rgbGuidanceAvailable,rgbGuidanceWidth:this.rgbGuidanceWidth,
      rgbGuidanceHeight:this.rgbGuidanceHeight,visualEvidenceFrames,monocularPriorStatus:optionalMonocularPrior.status,motionState:normalFinish?this.motionState:'paused',error:null});
    const diagnostics={evidenceEpoch:this.evidenceEpoch,depthFrames:snapshot?.depthFrameCount??this.depthFrameCount,integratedFrames:snapshot?.sampledDepthFrames??this.sampledDepthFrames,stablePoints:pointCount,retainedPoints:retainedPointCount,keyframes:keyframeCount,polishFrames:polishFrameCount,weak:this.coverageWeakPoints,improving:this.coverageImprovingPoints,stable:this.coverageStablePoints,manualPoints:snapshot?.manualControlPoints.length??this.manualPoints.size,operatorEdges:snapshot?.operatorEdges.length??this.operatorEdges.size,visualEvidenceFrames,monocularPriorStatus:optionalMonocularPrior.status,monocularPriorCalibration:Number(optionalMonocularPrior.calibrationConfidence.toFixed(3)),performance};
    if(snapshot!==null)Object.assign(diagnostics,{tentativePoints:snapshot.tentativePointCount,residualMm:Math.round(snapshot.averageStableResidualMeters*1000)});
    clientDiagnostics.info(normalFinish?'XR_SCAN_FINISHED':'XR_SCAN_INTERRUPTED',diagnostics);void clientDiagnostics.flush();
  }

  private refreshCoverageGuide(time:number,force=false):void{
    const adaptiveIntervalMs=Math.max(COVERAGE_GPU_UPLOAD_INTERVAL_MS,this.performanceProfiler.recommendedCoverageIntervalMs());if(!force&&time-this.lastCoverageGpuUploadMs<adaptiveIntervalMs)return;
    this.lastCoverageGpuUploadMs=time;const started=performance.now();
    try{
      const manualPositions=this.manualPoints.xrReferencePositions();const pointBudget=this.performanceProfiler.renderPolicy().pointBudget;
      const regularBudget=Math.max(0,pointBudget-Math.floor(manualPositions.length/3));
      const coverage=this.pointCloud.liveCoverageSnapshot(regularBudget);const counts=countCoverageQualityBands(coverage.confidence.subarray(0,coverage.pointCount));
      this.coverageGuideActive=coverage.active&&this.coverageRenderer!==null;this.coverageVisibleCells=coverage.visibleCoverageCells;this.coverageReadyCells=coverage.readyCoverageCells;this.coverageTotalCells=coverage.totalCoverageCells;
      this.coverageTargetIndependentViews=coverage.targetFrameHits;this.coverageAverageConfidence=coverage.averageCoverageConfidence;this.coverageWeakPoints=counts.weak;this.coverageImprovingPoints=counts.improving;this.coverageStablePoints=counts.stable;
      if(this.coverageRenderer!==null){const renderSnapshot=composeCoverageRenderSnapshot(coverage,manualPositions,this.coverageCompositionBuffers);if(renderSnapshot.pointCount>0)this.coverageRenderer.update(renderSnapshot);else this.coverageRenderer.clear();this.coverageGuideRenderPoints=coverage.pointCount;}else this.coverageGuideRenderPoints=0;
      this.performanceProfiler.record('coverage',performance.now()-started);
    }catch(error){this.disableCoverageGuide(error);}
  }

  private updateLightEstimate(time:number,frame:XrFrameLike):void{
    if(this.lightProbe===null||typeof frame.getLightEstimate!=='function'||time-this.lastLightEstimateMs<LIGHT_ESTIMATE_INTERVAL_MS)return;this.lastLightEstimateMs=time;
    try{const estimate=frame.getLightEstimate(this.lightProbe);if(estimate===null)return;const i=estimate.primaryLightIntensity;const magnitude=Math.sqrt((i.x*i.x+i.y*i.y+i.z*i.z)/3);if(Number.isFinite(magnitude)){this.lightingEstimateAvailable=true;this.primaryLightIntensity=magnitude;}}catch{/*advisory*/}
  }

  private acceptRgbGuidance(guidance:ReturnType<RawCameraGuidanceCapture['pollReady']>):void{
    if(guidance===null)return;setLatestRgbGuidance(guidance);this.rgbGuidanceAvailable=true;this.rgbGuidanceWidth=guidance.width;this.rgbGuidanceHeight=guidance.height;
  }

  private captureRgbGuidance(time:number,view:XrViewLike):void{
    const capture=this.cameraCapture;if(capture===null)return;
    // WebGL2 PBO readback is polled every RAF with timeout=0, independently of
    // the lower capture cadence. This avoids both GPU stalls and a 520 ms
    // artificial delivery delay.
    try{this.acceptRgbGuidance(capture.pollReady());}catch(error){this.disableCameraGuidance('rgb',error,'rgb-camera-readback');return;}
    if(this.cameraBinding===null||view.camera==null||time-this.lastRgbCaptureMs<RGB_CAPTURE_INTERVAL_MS)return;this.lastRgbCaptureMs=time;
    try{this.acceptRgbGuidance(capture.capture(this.cameraBinding,view,time));}
    catch(error){this.disableCameraGuidance('rgb',error,'rgb-camera-capture');}
  }

  private acceptTagFrame(frame:TagCameraFrame|null):void{
    if(frame===null||this.evidenceFrozen)return;
    const listeners=[...this.tagFrameListeners];
    // Transferring the first owned frame detaches its ArrayBuffer. Prepare one
    // explicit copy per additional subscriber before any listener can transfer.
    const values=listeners.map((_,index)=>index===0||frame.rgbaOwnership!=='owned'?frame:{...frame,rgba:frame.rgba.slice()});
    for(let index=0;index<listeners.length;index+=1)listeners[index](values[index]);
  }

  private captureTagGuidance(time:number,view:XrViewLike):void{
    if(this.evidenceFrozen||this.originResetPaused||!this.tagCaptureEnabled)return;
    const capture=this.tagCameraCapture;if(capture===null)return;
    try{this.acceptTagFrame(capture.pollReadyTag());}catch(error){this.disableCameraGuidance('tag',error,'tag-camera-readback');return;}
    if(this.cameraBinding===null||view.camera==null||time-this.lastTagCaptureMs<TAG_CAPTURE_INTERVAL_MS)return;
    this.lastTagCaptureMs=time;
    /* The capture target follows the camera image aspect (same pixel budget), so tags are not stretched. */
    const wanted=tagCaptureSizeFor(view.camera.width,view.camera.height);
    if(wanted.width!==this.tagCaptureWidth||wanted.height!==this.tagCaptureHeight){
      try{
        const gl=this.gl;if(gl===null)throw new Error('No WebGL context for the tag capture.');
        const next=new RawCameraGuidanceCapture(gl,wanted.width,wanted.height);
        if(!isTagReadbackPerformanceSafe(next.tagReadbackMode)){next.dispose();this.disableCameraGuidance('tag',new Error('Non-blocking readback is unavailable at the camera-matched tag capture size.'),'tag-camera-resize');return;}
        capture.dispose();this.tagCameraCapture=next;
        clientDiagnostics.info('XR_TAG_CAPTURE_RESIZED',{fromWidth:this.tagCaptureWidth,fromHeight:this.tagCaptureHeight,width:wanted.width,height:wanted.height,cameraWidth:view.camera.width,cameraHeight:view.camera.height});void clientDiagnostics.flush();
        this.tagCaptureWidth=wanted.width;this.tagCaptureHeight=wanted.height;
      }catch(error){this.disableCameraGuidance('tag',error,'tag-camera-resize');}
      return;
    }
    if(!this.tagGeometryLogged){
      /* Once per session: does the camera image share the aspect/FOV of the view the pose maths assumes? A mismatch would offset anchored tags. */
      this.tagGeometryLogged=true;
      const projection=view.projectionMatrix;const p00=projection===undefined?NaN:Number(projection[0]);const p11=projection===undefined?NaN:Number(projection[5]);
      const round=(value:number,digits:number)=>Number.isFinite(value)?Math.round(value*10**digits)/10**digits:null;
      clientDiagnostics.info('XR_TAG_CAMERA_GEOMETRY',{cameraWidth:view.camera.width,cameraHeight:view.camera.height,cameraAspect:round(view.camera.width/view.camera.height,4),
        captureWidth:this.tagCaptureWidth,captureHeight:this.tagCaptureHeight,projectionAspect:round(p11/p00,4),fovYDeg:round(2*Math.atan(1/p11)*180/Math.PI,2),fovXDeg:round(2*Math.atan(1/p00)*180/Math.PI,2),
        window:typeof window!=='undefined'?`${window.innerWidth}x${window.innerHeight}`:null,orientation:typeof screen!=='undefined'&&screen.orientation?screen.orientation.type:null});void clientDiagnostics.flush();
    }
    try{this.acceptTagFrame(capture.captureTag(this.cameraBinding,view,time));}
    catch(error){this.disableCameraGuidance('tag',error,'tag-camera-capture');}
  }

  private captureNativePlaneGuidance(time:number,frame:XrFrameLike,referenceSpace:XrReferenceSpaceLike):void{
    if(time-this.lastPlaneGuidanceMs<PLANE_GUIDANCE_INTERVAL_MS)return;
    this.lastPlaneGuidanceMs=time;
    try{captureLatestStructuralPlanePriors(frame,referenceSpace);}
    catch(error){resetXrPlaneGuidanceRegistry();this.recordFrameFault('native-plane-guidance',error);}
  }

  private maybeRecordVisualEvidence(time:number,keyframeId:number,view:XrViewLike):void{
    if(time-this.lastVisualEvidenceMs<VISUAL_EVIDENCE_INTERVAL_MS)return;const guidance=getFreshRgbGuidance(time,300);if(guidance===null)return;
    const detected=detectVisualLines(guidance);this.lastVisualEvidenceMs=time;
    addVisualEvidenceFrame({keyframeId,capturedAtMs:guidance.capturedAtMs,lines:detected.lines,junctions:detected.junctions,
      projectionMatrix:guidance.projectionMatrix??Float32Array.from(view.projectionMatrix),
      worldFromView:guidance.worldFromView??Float32Array.from(view.transform.matrix)});
  }

  /**
   * Issue #45: a Continue scan must not feed depth into the saved scan before the new session is tied to it by known tags. While the gate is closed
   * depth frames are dropped (tag detection, tracking and the camera keep running, so the tags can still be found). The owner opens it on a lock.
   */
  setDepthGate(blocked:boolean):void{this.depthGateBlocked=blocked;}

  /**
   * The translucent mesh over the camera while scanning. 'auto' runs it only while the phone has headroom (low performance pressure); 'on' runs it whatever
   * the pressure; 'off' never. The worker build is throttled and never queued, so a busy phone gets a slower preview, not a slower scan.
   */
  setLiveMeshMode(mode:LiveMeshMode):void{
    if(mode!==this.liveMeshMode||!this.liveMeshModeLogged){this.liveMeshModeLogged=true;clientDiagnostics.info('XR_LIVE_MESH_MODE_SET',{mode});}
    this.liveMeshMode=mode;
    if(mode==='off')this.stopLiveMesh();
  }
  /**
   * The saved scan's mesh, already in this session's WebXR frame (Continue, once the tags tie the sessions together). A view only; null removes it.
   * Drawn in amber, below the blue mesh being built now. Not drawn while an origin reset is pending: it would be in the wrong frame.
   */
  setReferenceMesh(mesh:ReferenceMeshOverlay|null):void{
    this.referenceMesh=mesh;this.referenceMeshDirty=mesh!==null;
    if(mesh===null)this.referenceMeshRenderer?.clear();
  }
  private liveMeshWanted():boolean{
    if(this.liveMeshOff||this.liveMeshMode==='off'||this.scanPaused||this.originResetPaused||this.gl===null)return false;
    return true;
  }
  /** 'on' builds at the normal pace. 'auto' follows the phone: the busier it is, the less often (never switched off by pressure alone: a phone that is busy while scanning is the normal case). */
  private liveMeshIntervalMs():number{
    if(this.liveMeshMode==='on')return LIVE_MESH_MIN_INTERVAL_MS;
    const pressure=this.livePerformancePressure();
    return pressure==='low'?LIVE_MESH_MIN_INTERVAL_MS:pressure==='medium'?LIVE_MESH_MIN_INTERVAL_MS*2:LIVE_MESH_MIN_INTERVAL_MS*3.5;
  }
  private tickReferenceMesh(layer:XrWebGlLayerLike,views:readonly CoverageRenderView[]):void{
    const mesh=this.referenceMesh;
    if(mesh===null||this.originResetPaused||this.gl===null)return;
    try{
      if(this.referenceMeshRenderer===null)this.referenceMeshRenderer=new LiveMeshRenderer(this.gl,{style:REFERENCE_MESH_STYLE,depthBias:true});
      if(this.referenceMeshDirty){this.referenceMeshRenderer.update(mesh.trianglesXr,mesh.triangleCount);this.referenceMeshDirty=false;}
      this.referenceMeshRenderer.render(layer as never,views);
    }catch(error){
      this.referenceMesh=null;try{this.referenceMeshRenderer?.dispose();}catch{/* already unusable */}this.referenceMeshRenderer=null;clientDiagnostics.warn('XR_REFERENCE_MESH_FAILED',error);
    }
  }
  private livePerformancePressure():'low'|'medium'|'high'{return activeXrPerformancePressure();}
  private stopLiveMesh():void{
    this.liveMeshPending=null;this.liveMeshRenderer?.clear();this.liveMeshLoggedActive=false;
  }
  private disposeLiveMesh():void{
    try{this.liveMeshClient?.dispose();}catch{/* already gone */}this.liveMeshClient=null;
    try{this.liveMeshRenderer?.dispose();}catch{/* already unusable */}this.liveMeshRenderer=null;this.liveMeshPending=null;this.liveMeshLoggedActive=false;
  }
  private tickLiveMesh(layer:XrWebGlLayerLike,views:readonly CoverageRenderView[]):void{
    if(!this.liveMeshWanted()){if(this.liveMeshRenderer!==null&&this.liveMeshRenderer.triangleCount>0)this.liveMeshRenderer.clear();return;}
    try{
      if(this.liveMeshClient===null){
        this.liveMeshClient=new LiveMeshClient({
          onResult:(result,elapsedMs)=>{this.liveMeshPending={trianglesXr:result.trianglesXr,triangleCount:result.triangleCount};if(!this.liveMeshLoggedActive){this.liveMeshLoggedActive=true;clientDiagnostics.info('XR_LIVE_MESH_ACTIVE',{triangles:result.triangleCount,points:result.acceptedPointCount,buildMs:Math.round(elapsedMs),mode:this.liveMeshMode});}},
          onDisabled:(reason)=>{this.liveMeshOff=true;this.stopLiveMesh();clientDiagnostics.warn('XR_LIVE_MESH_DISABLED',{reason});},
        });
      }
      this.liveMeshClient.request(()=>this.pointCloud.liveMeshInput(LIVE_MESH_MAX_POINTS),this.liveMeshIntervalMs());
      if(this.liveMeshRenderer===null&&this.gl!==null)this.liveMeshRenderer=new LiveMeshRenderer(this.gl);
      const renderer=this.liveMeshRenderer;
      if(renderer===null)return;
      const pending=this.liveMeshPending;
      if(pending!==null){this.liveMeshPending=null;renderer.update(pending.trianglesXr,pending.triangleCount);}
      renderer.render(layer as never,views);
    }catch(error){
      this.liveMeshOff=true;this.disposeLiveMesh();clientDiagnostics.warn('XR_LIVE_MESH_FAILED',error);
    }
  }

  /**
   * Issue #45 gate 2: the saved scan's measured points, already in this session's WebXR frame, drawn over the camera after the live coverage. Null removes it.
   * It is a view only: nothing is added to the scan. It is not drawn while an origin reset is pending (it would be in the wrong frame).
   */
  setReferenceOverlay(overlay:ReferenceOverlay|null):void{
    this.referenceOverlay=overlay;this.referenceOverlayDirty=overlay!==null;
    if(overlay===null)this.referenceRenderer?.clear();
  }

  private captureDepth(time:number,frame:XrFrameLike,pose:XrViewerPoseLike):void{
    if(this.depthGateBlocked||this.scanPaused||this.originResetPaused||!this.depthEnabled||pose.views.length===0)return;
    if(typeof frame.getDepthInformation!=='function'){if(!this.depthFailureReported){this.depthFailureReported=true;this.publish({depthError:'XRFrame.getDepthInformation() is unavailable.'});}return;}
    const view=pose.views[0];
    try{
      const depthStarted=performance.now();const depth=frame.getDepthInformation(view);this.performanceProfiler.record('depthAccess',performance.now()-depthStarted);if(depth===null)return;
      this.depthFrameCount+=1;this.depthWidth=depth.width;this.depthHeight=depth.height;
      if(!this.depthReadyLogged){this.depthReadyLogged=true;clientDiagnostics.info('XR_DEPTH_STREAM_READY',{width:depth.width,height:depth.height,usage:this.state.depthUsage,format:this.state.depthDataFormat,type:this.state.depthType});void clientDiagnostics.flush();}
      if(time-this.lastDepthSampleMs<DEPTH_SAMPLE_INTERVAL_MS)return;this.lastDepthSampleMs=time;
      const decision=this.keyframeSelector.evaluate(time,pose.transform);this.motionState=decision.state;this.motionQuality=decision.motionQuality;this.linearSpeedMetersPerSecond=decision.linearSpeedMetersPerSecond;this.angularSpeedRadiansPerSecond=decision.angularSpeedRadiansPerSecond;this.linearAccelerationMetersPerSecond2=decision.linearAccelerationMetersPerSecond2;
      if(decision.kind==='skip'){if(decision.state==='too-fast')this.motionRejectedFrames+=1;return;}
      this.sampledDepthFrames+=1;if(decision.kind==='keyframe')this.maybeRecordVisualEvidence(time,decision.keyframeId,view);
      const context:DepthFusionFrameContext={keyframeId:decision.keyframeId,evidenceKeyframe:decision.kind==='keyframe',frameWeight:decision.frameWeight,cameraProjectPosition:projectCameraPosition(pose)};
      const fusionStarted=performance.now();const result=this.pointCloud.sampleDepthFrame(depth,depthGeometry(depth,view),context);this.performanceProfiler.record('fusion',performance.now()-fusionStarted);if(result.changedPoints>0)this.pointCloudVersion+=1;this.refreshCoverageGuide(time);
    }catch(error){if(!this.depthFailureReported){this.depthFailureReported=true;const message=`Depth access failed: ${errorText(error)}`;clientDiagnostics.error('XR_DEPTH_ACCESS_FAILED',message);this.publish({depthError:message});}}
  }

  private reportClientTelemetry(time:number):void{
    if(time-this.lastClientTelemetryMs<CLIENT_TELEMETRY_INTERVAL_MS)return;this.lastClientTelemetryMs=time;
    clientDiagnostics.info('XR_PROGRESS',{frames:this.frameCount,depthFrames:this.depthFrameCount,integratedFrames:this.sampledDepthFrames,stablePoints:this.pointCloud.promotedCount,retainedPoints:this.pointCloud.pointCount,keyframes:this.pointCloud.keyframeCount,polishFrames:this.pointCloud.polishFrameCount,readyCells:this.coverageReadyCells,visibleCells:this.coverageVisibleCells,weak:this.coverageWeakPoints,improving:this.coverageImprovingPoints,stable:this.coverageStablePoints,manualPoints:this.manualPoints.size,operatorEdges:this.operatorEdges.size,motion:this.motionState,rgbGuidance:this.rgbGuidanceAvailable,rgbReadback:this.cameraCapture?.usesNonBlockingReadback?'pbo-fence':'sync-or-unavailable',visualEvidenceFrames:getVisualEvidenceFrameCount(),nativePlanePriors:getLatestStructuralPlanePriorCount(),monocularPriorStatus:optionalMonocularPrior.status,monocularPriorCalibration:Number(optionalMonocularPrior.calibrationConfidence.toFixed(3)),performance:this.performanceProfiler.snapshot()});void clientDiagnostics.flush();
  }

  private readonly handleFrame:XrFrameCallback=(time,frame):void=>{
    const session=this.session,referenceSpace=this.referenceSpace,gl=this.gl,layer=this.layer;if(session===null||referenceSpace===null||gl===null||layer===null||frame.session!==session)return;
    this.rafHandle=null;
    const generation=this.lifecycleGeneration;let faultClass='xr-frame-body';
    try{
      if(this.evidenceFrozen){
        faultClass='xr-core-gl';gl.bindFramebuffer(gl.FRAMEBUFFER,layer.framebuffer);gl.viewport(0,0,layer.framebufferWidth,layer.framebufferHeight);gl.clearColor(0,0,0,0);gl.clearDepth(1);gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT);
        if(this.session===session&&this.lifecycleGeneration===generation&&(this.state.phase==='stopping'||this.state.phase==='running')){faultClass='raf-schedule';this.rafHandle=session.requestAnimationFrame(this.handleFrame);}
        return;
      }
      this.performanceProfiler.recordFrameTimestamp(time);const frameWorkStarted=performance.now();
      // WebXR requires pauseDepthSensing()/resumeDepthSensing() while this current
      // XR animation frame is active. Visibility handlers only change scanPaused.
      this.syncDepthSensingPauseInsideFrame();
      this.captureNativePlaneGuidance(time,frame,referenceSpace);
      faultClass='xr-core-gl';
      gl.bindFramebuffer(gl.FRAMEBUFFER,layer.framebuffer);gl.viewport(0,0,layer.framebufferWidth,layer.framebufferHeight);gl.clearColor(0,0,0,0);gl.clearDepth(1);gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT);
      faultClass='viewer-pose';
      const pose=frame.getViewerPose(referenceSpace);
      faultClass='xr-frame-body';
      if(pose!==null&&pose.views.length>0){const view=pose.views[0];for(let index=0;index<16;index+=1){this.latestProjectionMatrix[index]=Number(view.projectionMatrix[index]);this.latestWorldFromView[index]=Number(view.transform.matrix[index]);}this.hasLatestProjection=true;setLatestProjectionMatrices(this.latestProjectionMatrix,this.latestWorldFromView);this.captureRgbGuidance(time,view);this.captureTagGuidance(time,view);}
      this.frameCount+=1;this.updateLightEstimate(time,frame);
      if(pose!==null&&!this.scanPaused){this.captureDepth(time,frame,pose);const renderer=this.coverageRenderer;if(renderer!==null){try{renderer.render(layer,pose.views);}catch(error){this.disableCoverageGuide(error);}}
        this.tickReferenceMesh(layer,pose.views);
        this.tickLiveMesh(layer,pose.views);
        const overlay=this.referenceOverlay;
        if(overlay!==null&&!this.originResetPaused&&this.gl!==null){
          try{
            if(this.referenceRenderer===null)this.referenceRenderer=new LiveCoveragePointRenderer(this.gl);
            if(this.referenceOverlayDirty){this.referenceRenderer.update(overlay);this.referenceOverlayDirty=false;}
            this.referenceRenderer.render(layer,pose.views);
          }catch(error){this.referenceOverlay=null;try{this.referenceRenderer?.dispose();}catch{/* already unusable */}this.referenceRenderer=null;clientDiagnostics.warn('XR_REFERENCE_OVERLAY_FAILED',error);}
        }
      }
      this.performanceProfiler.record('frameWork',performance.now()-frameWorkStarted);this.reportClientTelemetry(time);
      if(time-this.lastUiUpdateMs>=100||this.frameCount<=1){
        this.lastUiUpdateMs=time;
        const commonPatch={frameCount:this.frameCount,scanPaused:this.scanPaused||this.originResetPaused,originReset:this.originResetPaused,wakeLockActive:this.wakeLock!==null&&!this.wakeLock.released,depthWidth:this.depthWidth,depthHeight:this.depthHeight,depthFrameCount:this.depthFrameCount,sampledDepthFrames:this.sampledDepthFrames,pointCount:this.pointCloud.promotedCount,retainedPointCount:this.pointCloud.pointCount,pointCloudVersion:this.pointCloudVersion,keyframeCount:this.pointCloud.keyframeCount,polishFrameCount:this.pointCloud.polishFrameCount,motionRejectedFrames:this.motionRejectedFrames,motionState:this.scanPaused?'paused' as const:this.motionState,motionQuality:this.motionQuality,linearSpeedMetersPerSecond:this.linearSpeedMetersPerSecond,angularSpeedRadiansPerSecond:this.angularSpeedRadiansPerSecond,linearAccelerationMetersPerSecond2:this.linearAccelerationMetersPerSecond2,coverageGuideActive:this.coverageGuideActive,coverageGuideRenderPoints:this.coverageGuideRenderPoints,coverageVisibleCells:this.coverageVisibleCells,coverageReadyCells:this.coverageReadyCells,coverageTotalCells:this.coverageTotalCells,coverageTargetIndependentViews:this.coverageTargetIndependentViews,coverageAverageConfidence:this.coverageAverageConfidence,coverageWeakPoints:this.coverageWeakPoints,coverageImprovingPoints:this.coverageImprovingPoints,coverageStablePoints:this.coverageStablePoints,coverageGuideError:this.coverageGuideError,manualPointCount:this.manualPoints.size,manualEdgeCount:this.operatorEdges.size,performance:this.performanceProfiler.snapshot(),lightingEstimateAvailable:this.lightingEstimateAvailable,primaryLightIntensity:this.primaryLightIntensity,rgbGuidanceAvailable:this.rgbGuidanceAvailable,rgbGuidanceWidth:this.rgbGuidanceWidth,rgbGuidanceHeight:this.rgbGuidanceHeight,visualEvidenceFrames:getVisualEvidenceFrameCount(),monocularPriorStatus:optionalMonocularPrior.status};
        if(pose===null)this.publish({...commonPatch,tracking:false,pose:null});
        else{const{position,orientation}=pose.transform;this.publish({...commonPatch,tracking:true,pose:{position:[position.x,position.y,position.z],orientation:[orientation.x,orientation.y,orientation.z,orientation.w],views:pose.views.length}});}
      }
      if(this.session===session&&this.lifecycleGeneration===generation&&this.state.phase==='running'){
        faultClass='raf-schedule';this.rafHandle=session.requestAnimationFrame(this.handleFrame);
      }
    }catch(error){this.handleFatalFrameFault(session,generation,error,faultClass);}
  };
}
